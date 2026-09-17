/* config.json is hand-edited by every user, per the README — and a mistyped
   value used to be indistinguishable from an absent one, because json_get_str()
   returns NULL for both. Quoting the port by mistake silently gave you 8765 and
   a "connection refused" with nothing pointing at the config.

   Runs the real sync_push with LAN_FOLDER_SYNC_CONFIG pointed at each case, so
   it exercises the actual startup path. Needs no server: every case here fails
   (or reaches the push banner) before a socket is opened. */
#include "harness.h"

static char cfg_path[256];

static void cleanup(void)
{
    unlink(cfg_path);
}

/* Write a config, run sync_push, return its combined output. */
static char *run_with(const char *json, int *status)
{
    FILE *f = fopen(cfg_path, "w");
    CHECK(f != NULL, "cannot write %s: %s", cfg_path, strerror(errno));
    fputs(json, f);
    fclose(f);

    char cmd[768];
    snprintf(cmd, sizeof(cmd),
             "LAN_FOLDER_SYNC_CONFIG=%s ./bin/sync_push 127.0.0.1 "
             "/nonexistent-dir-for-config-test 2>&1",
             cfg_path);
    return run_capture(cmd, status);
}

/* A malformed value must name itself, not fall through to a default. */
static void expect_message(const char *label, const char *json,
                           const char *needle)
{
    int status = 0;
    char *out = run_with(json, &status);
    CHECK(status != 0, "%s: accepted a bad config (exit 0)\n%s", label, out);
    CHECK(strstr(out, needle) != NULL,
          "%s: expected '%s', got:\n%s", label, needle, out);
    ok("%s -> %s", label, needle);
    free(out);
}

int main(void)
{
    snprintf(cfg_path, sizeof(cfg_path), "sandbox/_config_test_%d.json",
             (int)getpid());
    mkdir_p("sandbox");
    atexit(cleanup);

    section("config.json validation");

    expect_message("token as a number", "{\"token\": 12345}",
                   "must be a string");
    expect_message("token absent", "{\"shared_dir\": \"x\"}",
                   "missing \"token\"");
    expect_message("shared_dir as a list",
                   "{\"token\":\"t\",\"shared_dir\":[\"/Users/me\"]}",
                   "must be a string");
    expect_message("peer as a string", "{\"token\":\"t\",\"peer\":\"nope\"}",
                   "\"peer\" must be an object");
    expect_message("peer.host as a number",
                   "{\"token\":\"t\",\"peer\":{\"host\":42}}",
                   "\"peer.host\" must be a string");
    expect_message("port in quotes",
                   "{\"token\":\"t\",\"peer\":{\"port\":\"9999\"}}",
                   "must be a number, not in quotes");
    expect_message("port out of range",
                   "{\"token\":\"t\",\"peer\":{\"port\":0}}",
                   "must be 1-65535");
    expect_message("not JSON at all", "hello", "is not valid JSON");

    section("Valid configs still load");

    /* The push gets as far as its banner, which proves the config parsed and
       the values landed where they should. */
    int status = 0;
    char *out = run_with("{\"token\":\"t\",\"peer\":{\"port\":9999}}", &status);
    CHECK(strstr(out, "127.0.0.1:9999") != NULL,
          "a valid port did not reach the peer address:\n%s", out);
    ok("a valid port is used verbatim");
    free(out);

    /* Optional keys may be absent - that is not the same as mistyped. */
    out = run_with("{\"token\":\"t\"}", &status);
    CHECK(strstr(out, "127.0.0.1:8765") != NULL,
          "an absent peer block should fall back to the default:\n%s", out);
    ok("absent optional keys still fall back to defaults");
    free(out);

    done("a mistyped config names itself instead of silently defaulting \xe2\x9a\x99\xef\xb8\x8f");
    return 0;
}
