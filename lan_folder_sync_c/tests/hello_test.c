/* HELLO gates every op: a wrong or missing token is refused and the session is
   closed before anything is served. Mirrors tests/hello_test.py. */
#include "harness.h"

/* Send `frame` as the opening frame; expect an ERROR, then a closed session. */
static void expect_rejected(const char *label, const char *frame)
{
    int s = dial_test_server();
    send_json(s, frame);

    json_value *msg = recv_json_frame(s);
    CHECK(msg != NULL, "%s: server closed without sending an error", label);
    const char *op = json_get_str(msg, "op");
    CHECK(op != NULL && strcmp(op, "ERROR") == 0, "%s: expected ERROR", label);

    /* the session must be gone — a second read hits EOF */
    char *extra = NULL;
    CHECK(recv_msg(s, &extra, NULL) != FRAME_OK,
          "%s: server did not close after rejecting", label);

    ok("%s: rejected \xe2\x86\x92 '%s', then closed", label, json_get_str(msg, "message"));
    json_free(msg);
    close(s);
}

int main(void)
{
    section("HELLO rejects the unauthenticated");

    /* --- Case 1: wrong token -> ERROR + close, before anything is served --- */
    expect_rejected("wrong-token",
                    "{\"op\": \"HELLO\", \"token\": \"definitely-not-it\"}");

    /* --- Case 2: missing token -> same treatment --- */
    expect_rejected("missing-token", "{\"op\": \"HELLO\"}");

    /* --- Case 3: skipping HELLO entirely (MANIFEST first) -> refused --- */
    expect_rejected("no-hello", "{\"op\": \"MANIFEST\"}");

    /* a mutating op with no HELLO must NOT act — refused before dispatch */
    expect_rejected("delete-before-hello",
                    "{\"op\": \"DELETE\", \"path\": \"anything.txt\"}");

    section("HELLO with the right token proceeds");

    /* --- Case 4: correct token -> OK, then the session actually serves --- */
    int s = dial_test_server();
    strbuf sb;
    sb_init(&sb);
    sb_addstr(&sb, "{\"op\": \"HELLO\", \"token\": ");
    json_escape(&sb, test_token());
    sb_addch(&sb, '}');
    send_msg(s, sb.data, sb.len);
    sb_free(&sb);

    json_value *msg = recv_json_frame(s);
    CHECK(msg != NULL, "server closed instead of accepting a valid token");
    const char *op = json_get_str(msg, "op");
    CHECK(op != NULL && strcmp(op, "OK") == 0, "expected OK");
    json_free(msg);
    ok("correct token accepted \xe2\x86\x92 OK");

    /* the same authenticated connection can now do real work */
    send_json(s, "{\"op\": \"MANIFEST\"}");
    msg = recv_json_frame(s);
    CHECK(msg != NULL, "authenticated session died before serving a manifest");
    op = json_get_str(msg, "op");
    CHECK(op != NULL && strcmp(op, "MANIFEST") == 0, "expected MANIFEST after HELLO");

    manifest m;
    manifest_from_json(json_get(msg, "files"), &m);
    ok("authenticated session served a manifest (%zu files)", m.count);
    manifest_free(&m);
    json_free(msg);
    close(s);

    done("HELLO gates every op; wrong/missing token is rejected and closed \xf0\x9f\x94\x90");
    return 0;
}
