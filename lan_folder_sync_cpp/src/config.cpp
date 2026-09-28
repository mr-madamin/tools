#include "config.hpp"

#include "json.hpp"
#include "util.hpp"

#include <cstdlib>
#include <format>
#include <unistd.h>

namespace lfs {
namespace {

std::optional<std::string> find_config(const char* argv0)
{
    if (const char* env = std::getenv("LAN_FOLDER_SYNC_CONFIG"); env != nullptr && *env != '\0')
        return std::string(env);

    if (access("config.json", R_OK) == 0)
        return std::string("config.json");

    if (argv0 != nullptr) {
        std::string dir = path_dirname(argv0);
        std::string here = path_join(dir, "config.json");
        if (access(here.c_str(), R_OK) == 0)
            return here;
        std::string up = path_join(dir, "../config.json");
        if (access(up.c_str(), R_OK) == 0)
            return up;
    }
    return std::nullopt;
}

// A key that is absent is fine -- the defaults are the point. A key that is
// present but the wrong type is a typo, and gets named.
std::optional<std::string> config_string(const json::Value& object, std::string_view key,
                                         std::string_view path, std::string_view where)
{
    const json::Value* v = object.find(key);
    if (v == nullptr || v->is_null())
        return std::nullopt; // genuinely absent -- the caller decides if that's ok
    const std::string* s = v->as_string();
    if (s == nullptr)
        throw ConfigError(std::format("{}: \"{}\" must be a string, in double quotes", path, where));
    return *s;
}

} // namespace

Config parse_config(std::string_view text, std::string_view path)
{
    auto root = json::parse(text);
    if (!root || !root->is_object())
        throw ConfigError(std::format("{} is not valid JSON", path));

    Config cfg;
    cfg.shared_dir = config_string(*root, "shared_dir", path, "shared_dir");

    auto token = config_string(*root, "token", path, "token");
    if (!token)
        throw ConfigError(std::format("{}: missing \"token\"", path));
    cfg.token = *token;

    const json::Value* peer = root->find("peer");
    if (peer != nullptr && !peer->is_null()) {
        if (!peer->is_object())
            throw ConfigError(std::format(
                "{}: \"peer\" must be an object, like "
                "{{\"host\": \"192.168.1.42\", \"port\": 8765}}",
                path));

        cfg.peer_host = config_string(*peer, "host", path, "peer.host");

        const json::Value* port = peer->find("port");
        if (port != nullptr && !port->is_null()) {
            const double* value = port->as_number();
            if (value == nullptr)
                throw ConfigError(std::format("{}: peer.port must be a number, not in quotes", path));
            // Same range check as the command line; a JSON double also has to
            // survive the cast to int, so reject inf/NaN before it.
            if (!(*value >= 1 && *value <= 65535))
                throw ConfigError(std::format("{}: peer.port must be 1-65535", path));
            cfg.peer_port = static_cast<int>(*value);
        }
    }

    return cfg;
}

Config load_config(const char* argv0)
{
    auto path = find_config(argv0);
    auto text = path ? read_whole_file(*path) : std::nullopt;

    if (!text)
        die("config.json not found. Copy config.example.json to config.json and set "
            "shared_dir, peer host/port, and a shared token (same on both machines).");

    try {
        return parse_config(*text, *path);
    } catch (const ConfigError& e) {
        die(e.what());
    }
}

} // namespace lfs
