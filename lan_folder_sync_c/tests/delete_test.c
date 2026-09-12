/* DELETE removes, previews safely under --dry-run, and refuses to escape the
   shared folder. Mirrors tests/delete_test.py. */
#include "harness.h"

#include <sys/stat.h>

#define SANDBOX  "sandbox"
#define ROOT_DIR SANDBOX "/source"
#define PEER_DIR SANDBOX "/received" /* same machine, so we can plant files */

static char probe[64];
static char victim[64];
static char victim_path[256]; /* a sibling of received/, i.e. OUTSIDE it */

/* Run the REAL sync_push with extra flags; assert it exited clean. */
static char *push(const char *extra_flags)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "./bin/sync_push %s %s %s 2>&1", TEST_HOST, ROOT_DIR,
             extra_flags);
    int status = 0;
    char *out = run_capture(cmd, &status);
    info(out);
    CHECK(status == 0, "push crashed (exit %d)", status);
    return out;
}

/* Simulate a file the peer has but the source does not (a delete candidate). */
static void plant_on_peer(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", PEER_DIR, name);
    write_text_file(path, "delete me\n");
}

static int peer_has(const char *name)
{
    manifest m;
    remote_manifest(&m);
    int found = manifest_has(&m, name);
    manifest_free(&m);
    return found;
}

/* Send one hand-built frame and read the reply; the honest client can never
   emit these paths. */
static json_value *send_raw_delete(const char *path)
{
    int s = connect_authed();
    strbuf sb;
    sb_init(&sb);
    sb_addstr(&sb, "{\"op\": \"DELETE\", \"path\": ");
    json_escape(&sb, path);
    sb_addch(&sb, '}');
    send_msg(s, sb.data, sb.len);
    sb_free(&sb);

    json_value *reply = recv_json_frame(s);
    close(s);
    return reply;
}

static void cleanup(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", PEER_DIR, probe);
    unlink(path);
    snprintf(path, sizeof(path), "%s/sub/%s", PEER_DIR, probe);
    unlink(path);
    unlink(victim_path);
    snprintf(path, sizeof(path), "%s/sub", PEER_DIR);
    rmdir(path); /* only succeeds if it's empty, which is what we want */
}

int main(void)
{
    snprintf(probe, sizeof(probe), "_delete_probe_%d.txt", (int)getpid());
    snprintf(victim, sizeof(victim), "_delete_victim_%d.txt", (int)getpid());
    snprintf(victim_path, sizeof(victim_path), "%s/%s", SANDBOX, victim);
    atexit(cleanup);

    mkdir_p(ROOT_DIR);
    mkdir_p(PEER_DIR);

    section("DELETE behavior");

    /* --- Case 1: --delete actually removes the file from the peer --- */
    plant_on_peer(probe);
    CHECK(peer_has(probe), "setup failed: probe not on peer");
    free(push("--delete"));
    CHECK(!peer_has(probe), "%s still on peer after --delete", probe);
    ok("--delete removed %s from the peer", probe);

    /* --- Case 2: --dry-run --delete previews the delete but does not remove --- */
    plant_on_peer(probe);
    char *out = push("--dry-run --delete");
    char expected[128];
    snprintf(expected, sizeof(expected), "DELETE %s", probe);
    CHECK(strstr(out, expected) != NULL, "dry-run did not list the delete");
    CHECK(peer_has(probe), "%s was deleted during a DRY RUN!", probe);
    free(out);
    ok("--dry-run --delete previewed but kept %s", probe);

    section("Path safety");

    /* --- Case 3: a path escaping received/ is refused; the file survives --- */
    write_text_file(victim_path, "must survive\n");

    char escape[128];
    snprintf(escape, sizeof(escape), "../%s", victim);
    json_value *reply = send_raw_delete(escape);
    CHECK(reply != NULL, "server closed without sending an ERROR");
    const char *op = json_get_str(reply, "op");
    CHECK(op != NULL && strcmp(op, "ERROR") == 0, "expected ERROR");
    CHECK(access(victim_path, F_OK) == 0,
          "path-escape DELETE removed a file outside received/!");
    ok("path escape refused ('%s'); %s survived", json_get_str(reply, "message"),
       victim);
    json_free(reply);

    /* absolute path is the other traversal vector — same guard must catch it */
    reply = send_raw_delete("/etc/hosts");
    op = reply != NULL ? json_get_str(reply, "op") : NULL;
    CHECK(op != NULL && strcmp(op, "ERROR") == 0,
          "absolute-path DELETE was not refused");
    json_free(reply);
    ok("absolute path also refused");

    section("Guard doesn't over-block \xc2\xb7 idempotency");

    /* --- Case 4: a legit nested-subdir delete works (guard mustn't over-block) --- */
    char nested[128];
    snprintf(nested, sizeof(nested), "sub/%s", probe);
    plant_on_peer(nested); /* creates received/sub/<probe> */
    CHECK(peer_has(nested), "setup failed: nested probe not on peer");
    free(push("--delete"));
    CHECK(!peer_has(nested), "%s survived --delete (guard too strict?)", nested);
    ok("--delete removed nested %s", nested);

    /* --- Case 5: deleting a missing file is idempotent (no ERROR, session lives) --- */
    int s = connect_authed();
    char gone[128];
    snprintf(gone, sizeof(gone), "_never_existed_%d.txt", (int)getpid());
    strbuf sb;
    sb_init(&sb);
    sb_addstr(&sb, "{\"op\": \"DELETE\", \"path\": ");
    json_escape(&sb, gone);
    sb_addch(&sb, '}');
    send_msg(s, sb.data, sb.len);
    sb_free(&sb);

    /* server succeeds silently (no reply) — prove it by reusing the connection */
    send_json(s, "{\"op\": \"MANIFEST\"}");
    reply = recv_json_frame(s);
    close(s);
    CHECK(reply != NULL, "session died after deleting a missing file");
    op = json_get_str(reply, "op");
    CHECK(op != NULL && strcmp(op, "MANIFEST") == 0, "expected MANIFEST");
    json_free(reply);
    ok("deleting a missing file is idempotent; session survived");

    done("DELETE removes, previews safely, and refuses to escape received/ \xf0\x9f\x97\x91\xef\xb8\x8f");
    return 0;
}
