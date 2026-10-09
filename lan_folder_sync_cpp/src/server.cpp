#include "server.hpp"

#include "framing.hpp"
#include "json.hpp"
#include "manifest.hpp"
#include "paths.hpp"
#include "util.hpp"

#include <cerrno>
#include <cstdio>
#include <format>
#include <unistd.h>

namespace lfs {

void serve_session(Socket& conn, const std::string& shared_dir, const std::string& token,
                   int idle_timeout, const Logger& log_fn)
{
    auto log = [&](const std::string& line) {
        if (log_fn)
            log_fn(line);
        else
            std::printf("%s\n", line.c_str());
    };

    // Refuse, then end the session. The ERROR is best-effort: the peer may
    // already be gone, and there is nothing more to do about that.
    auto refuse = [&](std::string_view message) {
        try {
            send_error(conn, message);
        } catch (const std::exception&) {
        }
    };

    conn.set_timeout(idle_timeout);
    bool authenticated = false;

    try {
        for (;;) {
            // Everything a client sends us is a short header; the big frame in
            // this protocol only ever travels the other way.
            std::optional<std::string> payload;
            try {
                payload = recv_msg(conn, MAX_CONTROL_FRAME);
            } catch (const FrameTooLarge& e) {
                log(std::format("Refused oversized frame ({} bytes, cap {})", e.length,
                                MAX_CONTROL_FRAME));
                return refuse("frame too large");
            }
            if (!payload)
                return; // peer closed

            auto header = decode_header(*payload);
            if (!header) {
                switch (header.error()) {
                case HeaderError::NotUtf8Json:
                    return refuse("malformed header: not valid UTF-8 JSON");
                // "123" is valid JSON but not a header. Worth its own message:
                // the parse succeeded, so blaming UTF-8 sends you hunting in
                // the wrong place.
                case HeaderError::NotObject:
                    return refuse("malformed header: not a JSON object");
                }
            }

            const std::string* op = header->str("op");

            if (!authenticated) {
                if (op == nullptr || *op != "HELLO")
                    return refuse("auth required: send HELLO first");
                const std::string* got = header->str("token");
                // std::string::operator== is a byte comparison, which is what
                // the filesystem and every other implementation of this
                // protocol mean. (Swift's String == would accept a differently
                // normalised spelling of the same token.)
                if (got == nullptr || *got != token)
                    return refuse("bad token");

                authenticated = true;
                send_json(conn, R"({"op": "OK"})");
                log("     HELLO ok, session authenticated");
                continue;
            }

            if (op != nullptr && *op == "MANIFEST") {
                // Partial here is far less dangerous than partial on the pusher
                // -- files we fail to list just get re-sent, never deleted --
                // but say so, because it is the same underlying problem.
                Manifest m = Manifest::scan(shared_dir);
                send_msg(conn, std::format(R"({{"op": "MANIFEST", "files": {}}})", m.to_json()));

                if (!m.complete)
                    log(std::format("Sent manifest ({} files) - WARNING: some directories "
                                    "under {} could not be read; the list is incomplete",
                                    m.files.size(), shared_dir));
                else if (!m.skipped_links.empty())
                    log(std::format("Sent manifest ({} files, {} symlink{} skipped)",
                                    m.files.size(), m.skipped_links.size(),
                                    m.skipped_links.size() == 1 ? "" : "s"));
                else
                    log(std::format("Sent manifest ({} files)", m.files.size()));
            } else if (op != nullptr && *op == "PUT") {
                try {
                    log(std::format("    received {}", recv_file_body(conn, shared_dir, *header)));
                } catch (const PutError& e) {
                    // Every kind ends the session: the body length is unknown
                    // or untrustworthy, so there is no next frame boundary.
                    return refuse(e.what());
                } catch (const SocketTimeout&) {
                    log(std::format("Session timed out: peer stalled mid-file after {}s",
                                    idle_timeout));
                    return;
                }
            } else if (op != nullptr && *op == "BYE") {
                log("Peer said BYE");
                return;
            } else if (op != nullptr && *op == "DELETE") {
                const json::Value* raw = header->find("path");
                if (raw == nullptr || raw->is_null())
                    return refuse("DELETE missing 'path'");

                // One shared guard for PUT and DELETE.
                const std::string* rel_path = raw->as_string();
                auto target = rel_path != nullptr ? safe_path(shared_dir, *rel_path) : std::nullopt;
                if (!target)
                    return refuse(std::format("unsafe path refused: {}", json::py_repr(raw)));

                if (::unlink(target->c_str()) == 0)
                    log(std::format("    deleted {}", *rel_path));
                else
                    log(std::format("    already gone {}", *rel_path)); // delete is idempotent
            } else {
                return refuse(std::format("unknown op: {}", json::py_repr(header->find("op"))));
            }
        }
    } catch (const SocketTimeout&) {
        log(std::format("Session timed out: peer idle for {}s, dropping it", idle_timeout));
    } catch (const std::exception& e) {
        log(std::format("Session error: {}", e.what()));
    }
}

} // namespace lfs
