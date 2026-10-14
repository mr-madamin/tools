// config.json is hand-edited by every user, so a mistyped value has to name
// itself rather than fall through to a default.
#include "config.hpp"
#include "test.hpp"

#include <string>

namespace {

// The message a bad config produces, or "" when it parsed.
std::string error_for(std::string_view text)
{
    try {
        (void)lfs::parse_config(text, "config.json");
        return "";
    } catch (const lfs::ConfigError& e) {
        return e.what();
    }
}

bool mentions(const std::string& message, std::string_view fragment)
{
    return message.find(fragment) != std::string::npos;
}

} // namespace

TEST(Config, reads_a_good_file)
{
    auto cfg = lfs::parse_config(R"({
      "shared_dir": "/Users/you/SyncMe",
      "peer": { "host": "192.168.1.42", "port": 8765 },
      "token": "a-long-random-string"
    })",
                                 "config.json");

    CHECK(cfg.shared_dir == "/Users/you/SyncMe", "shared_dir was not read");
    CHECK(cfg.peer_host == "192.168.1.42", "peer.host was not read");
    CHECK(cfg.peer_port == 8765, "peer.port was not read");
    CHECK(cfg.token == "a-long-random-string", "token was not read");
}

TEST(Config, an_absent_optional_key_is_fine)
{
    auto cfg = lfs::parse_config(R"({"token": "t"})", "config.json");
    CHECK(!cfg.shared_dir.has_value(), "an absent shared_dir invented a value");
    CHECK(!cfg.peer_host.has_value(), "an absent peer invented a host");
    CHECK(cfg.peer_port == 0, "an absent peer invented a port");
}

TEST(Config, a_missing_token_stops_everything)
{
    CHECK(mentions(error_for(R"({"shared_dir": "/tmp"})"), "missing \"token\""),
          "a config with no token was accepted");
}

TEST(Config, a_quoted_port_names_itself)
{
    // The failure this replaces: "port": "9999" silently became 8765, and the
    // user got "connection refused" with nothing pointing at config.json.
    CHECK(mentions(error_for(R"({"token": "t", "peer": {"port": "9999"}})"),
                   "peer.port must be a number, not in quotes"),
          "a quoted port was accepted");
}

TEST(Config, an_out_of_range_port_names_itself)
{
    CHECK(mentions(error_for(R"({"token": "t", "peer": {"port": 70000}})"),
                   "peer.port must be 1-65535"),
          "port 70000 was accepted");
}

TEST(Config, a_non_string_token_names_the_key)
{
    // The old failure mode reported `missing "token"` for a token that was
    // right there, sending you to look for a key you had already written.
    CHECK(mentions(error_for(R"({"token": 12345})"), "\"token\" must be a string"),
          "a numeric token was not named");
}

TEST(Config, a_peer_that_is_not_an_object_names_itself)
{
    CHECK(mentions(error_for(R"({"token": "t", "peer": "192.168.1.42"})"),
                   "\"peer\" must be an object"),
          "a string peer was accepted");
}

TEST(Config, invalid_json_says_so)
{
    CHECK(mentions(error_for("{not json"), "is not valid JSON"), "invalid JSON was accepted");
    CHECK(mentions(error_for("[1, 2]"), "is not valid JSON"), "a JSON array was accepted");
}
