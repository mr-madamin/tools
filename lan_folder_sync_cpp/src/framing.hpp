// Wire protocol: a 4-byte big-endian length prefix, then a UTF-8 JSON payload.
// A PUT frame is followed by exactly `size` raw body bytes. Byte-for-byte the
// protocol framing.py speaks, so a C++ peer and a Python (or C, or Swift) peer
// interoperate.
#pragma once

#include "json.hpp"
#include "socket.hpp"

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace lfs {

// The length prefix is 4 bytes, so a peer can claim up to 4 GB before sending a
// single byte of payload -- and the C code allocated it on the spot, before any
// authentication. Two ceilings, because the two directions carry very different
// frames: a MANIFEST lists every file in the folder, while everything else is a
// short header. The server only ever reads the short kind.
constexpr std::size_t MAX_FRAME = 64u * 1024 * 1024;        // a manifest of a huge folder
constexpr std::size_t MAX_CONTROL_FRAME = 1u * 1024 * 1024; // HELLO / PUT / DELETE / BYE

// size and mtime arrive as JSON doubles. static_cast<long long>(inf) is
// undefined behaviour in C++ exactly as in C -- "size": 1e999 is a one-line
// frame that UBSan flags outright -- and a negative size silently truncated the
// destination to zero while reporting success. Bound both before anything is
// cast, opened or written.
constexpr long long MAX_FILE_SIZE = 64LL * 1024 * 1024 * 1024; // 64 GiB per file
constexpr double MAX_MTIME = 1e15;                             // far past any real clock

// The peer declared a frame bigger than we will allocate. The payload is left
// unread, so the stream has no boundary to resync on: every caller ends the
// session.
struct FrameTooLarge : std::runtime_error {
    explicit FrameTooLarge(std::size_t length)
        : std::runtime_error("frame too large"), length(length) {}
    std::size_t length;
};

void send_msg(Socket& sock, std::string_view payload);
void send_json(Socket& sock, std::string_view json);
void send_error(Socket& sock, std::string_view message);

// std::nullopt when the peer closed. Throws FrameTooLarge / SocketTimeout /
// SocketError.
std::optional<std::string> recv_msg(Socket& sock, std::size_t max = MAX_FRAME);

enum class HeaderError {
    NotUtf8Json, // didn't decode, or didn't parse
    NotObject,   // parsed, but "123" and "[1,2]" are not headers
};
std::expected<json::Value, HeaderError> decode_header(std::string_view payload);

// The header declares `size`, and the body that follows is the only unframed
// part of the stream -- so the receiver finds the next frame by counting
// exactly that many bytes. Send one byte too few or too many and it reads the
// next header as file content: every later file in the session is silently
// corrupt. The file can change underneath us at any point, so the declared size
// is the contract, and send_file honours it even when the file stops matching.
// A local file we could not read, thrown only BEFORE the PUT header goes out
// -- so the stream is still intact and the pusher can skip this one file and
// carry on. The common case is a file deleted between the scan and the send.
// (The C port dies here instead, which throws away the rest of the push.)
struct LocalFileError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class SendResult {
    Ok,
    // The file changed size while we were reading it. A warning, not an error:
    // exactly `size` body bytes still went out, so the stream is intact and the
    // session can continue -- only that one file's content is suspect.
    Changed,
};
SendResult send_file(Socket& sock, const std::string& root_dir, const std::string& rel_path);
void send_delete(Socket& sock, std::string_view rel_path);

// A PUT we refuse on its header. Each kind ends the session, because the body
// length is either unknown or not to be trusted -- there is no way to find the
// next frame boundary.
struct PutError : std::runtime_error {
    enum class Kind {
        MissingField, // detail names the field
        OutOfRange,   // detail names the field
        UnsafePath,   // detail is the path, as the peer spelled it
    };
    PutError(Kind kind, std::string detail, const std::string& what)
        : std::runtime_error(what), kind(kind), detail(std::move(detail)) {}
    Kind kind;
    std::string detail;
};

// Receives the body into dest_dir and returns the relative path it landed at.
// Throws PutError, SocketTimeout (peer stalled mid-file) or SocketError (peer
// closed mid-file, or a local write failed).
std::string recv_file_body(Socket& sock, const std::string& dest_dir,
                           const json::Value& header);

// Client side of HELLO. std::nullopt when the peer closed instead of replying.
std::optional<json::Value> handshake(Socket& sock, std::string_view token);

} // namespace lfs
