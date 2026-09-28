// config.json: the token, the folder, and where the peer is. Hand-edited by
// every user, so a mistyped value names itself rather than falling through to
// a default -- quoting the port by mistake ("port": "9999") used to give you
// 8765 and then a "connection refused" with nothing pointing at the config.
#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lfs {

struct Config {
    std::optional<std::string> shared_dir; // absent: callers apply their own default
    std::optional<std::string> peer_host;
    int peer_port = 0; // 0 when absent
    std::string token;
};

// Thrown by parse_config; load_config turns it into a die(). Separating the two
// is what lets the tests assert on these messages without spawning a process.
struct ConfigError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

Config parse_config(std::string_view text, std::string_view path);

// Reads config.json and exits loudly if it isn't there. The binaries live in
// bin/, so the search is a little wider than Python's "next to the source
// file": $LAN_FOLDER_SYNC_CONFIG, then ./config.json, then next to the
// executable, then its parent.
Config load_config(const char* argv0);

} // namespace lfs
