// The passive side of the protocol. Lives in the library rather than in
// sync_server's main so the tests can run real sessions in-process, on an
// ephemeral port.
#pragma once

#include "socket.hpp"

#include <functional>
#include <string>

namespace lfs {

// The server handles one session at a time, so a peer that connects and then
// says nothing isn't just its own problem -- it locks out everyone else until
// the process is killed. Bound the wait, generously: a legitimate peer can be
// busy walking a large folder between frames.
constexpr int SESSION_TIMEOUT = 300;

using Logger = std::function<void(const std::string&)>;

// Serve one connection until the peer says BYE, errs, or goes quiet. Nothing a
// peer sends can take the process down: every refusal is an ERROR frame plus
// the end of *this* session.
void serve_session(Socket& conn, const std::string& shared_dir, const std::string& token,
                   int idle_timeout = SESSION_TIMEOUT, const Logger& log = nullptr);

} // namespace lfs
