/* Four malformed frames, one after another, and the server still serves.
   Mirrors tests/bad_frame_test.py. */
#include "harness.h"

/* Send one bad frame on a fresh connection; expect an ERROR frame back. */
static void expect_error(const char *label, const char *payload)
{
    int s = connect_authed();
    send_json(s, payload);

    json_value *msg = recv_json_frame(s);
    CHECK(msg != NULL, "%s: server closed WITHOUT sending an ERROR", label);
    const char *op = json_get_str(msg, "op");
    CHECK(op != NULL && strcmp(op, "ERROR") == 0, "%s: expected ERROR", label);

    ok("%s: got ERROR \xe2\x86\x92 '%s'", label, json_get_str(msg, "message"));
    json_free(msg);
    close(s);
}

int main(void)
{
    section("Malformed-frame hardening");

    /* --- Case 1: not JSON at all (malformed parse) --- */
    expect_error("bad-json", "not json at all");

    /* --- Case 2: valid JSON, but no "op" --- */
    expect_error("missing-op", "{\"foo\": 1}");

    /* --- Case 3: well-formed frame, unknown op (a typo) --- */
    expect_error("unknown-op", "{\"op\": \"MANFEST\"}");

    /* --- Case 4: PUT header missing size/mtime --- */
    expect_error("put-missing-fields", "{\"op\": \"PUT\", \"path\": \"x\"}");

    /* --- Case 5: a 4-byte length prefix claiming 4 GB, sent BEFORE the HELLO.
       The server must refuse on the header alone: allocating first would let an
       unauthenticated peer exhaust memory (and xmalloc dies) with four bytes. */
    {
        int s = dial_test_server(); /* deliberately NOT authenticated */
        unsigned char huge[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
        CHECK(send_all(s, huge, 4) == 0, "oversized-frame: send failed");

        json_value *msg = recv_json_frame(s);
        CHECK(msg != NULL, "oversized-frame: server closed without an ERROR");
        const char *op = json_get_str(msg, "op");
        CHECK(op != NULL && strcmp(op, "ERROR") == 0,
              "oversized-frame: expected ERROR, not a 4 GB allocation");
        ok("oversized-frame: refused pre-auth \xe2\x86\x92 '%s'",
           json_get_str(msg, "message"));
        json_free(msg);
        close(s);
    }

    /* --- Case 6: deeply nested JSON, again BEFORE the HELLO. Every '[' costs a
       stack frame in a recursive-descent parser, and the header is parsed before
       any authentication — 200 KB of them used to walk a release build off the
       end of its stack and kill the process. The parser must refuse by depth. */
    {
        int s = dial_test_server(); /* deliberately NOT authenticated */
        size_t n = 200000;
        char *deep = xmalloc(n);
        memset(deep, '[', n);
        CHECK(send_msg(s, deep, n) == 0, "deep-nesting: send failed");
        free(deep);

        json_value *msg = recv_json_frame(s);
        CHECK(msg != NULL,
              "deep-nesting: no ERROR came back — the server crashed on nesting");
        const char *op = json_get_str(msg, "op");
        CHECK(op != NULL && strcmp(op, "ERROR") == 0, "deep-nesting: expected ERROR");
        ok("deep-nesting: refused pre-auth without crashing");
        json_free(msg);
        close(s);
    }

    /* The limit has to reject the abusive case without rejecting real data:
       a manifest nests three deep, so anything sane must still parse. */
    {
        char shallow[256];
        int at = 0;
        for (int i = 0; i < 20; i++)
            at += snprintf(shallow + at, sizeof(shallow) - at, "[");
        at += snprintf(shallow + at, sizeof(shallow) - at, "1");
        for (int i = 0; i < 20; i++)
            at += snprintf(shallow + at, sizeof(shallow) - at, "]");

        json_value *v = json_parse(shallow, strlen(shallow));
        CHECK(v != NULL, "20-deep nesting was rejected — the limit is too tight");
        json_free(v);
        ok("legitimate nesting (20 deep) still parses");
    }

    /* --- The real pass condition: still alive, still speaking the protocol --- */
    int s = connect_authed();
    send_json(s, "{\"op\": \"MANIFEST\"}");
    json_value *msg = recv_json_frame(s);
    CHECK(msg != NULL, "server died — no reply after five bad frames");
    const char *op = json_get_str(msg, "op");
    CHECK(op != NULL && strcmp(op, "MANIFEST") == 0, "expected MANIFEST");

    manifest m;
    manifest_from_json(json_get(msg, "files"), &m);
    ok("survived: server still served a manifest (%zu files)", m.count);
    manifest_free(&m);
    json_free(msg);
    close(s);

    done("server shrugged off every bad frame and kept serving \xf0\x9f\x9b\xa1\xef\xb8\x8f");
    return 0;
}
