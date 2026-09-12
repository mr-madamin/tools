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

    /* --- The real pass condition: still alive, still speaking the protocol --- */
    int s = connect_authed();
    send_json(s, "{\"op\": \"MANIFEST\"}");
    json_value *msg = recv_json_frame(s);
    CHECK(msg != NULL, "server died — no reply after four bad frames");
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
