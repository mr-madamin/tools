/* A source listing we know is incomplete must not drive deletions.

   walk() skips what it cannot read — a directory without permission, or a path
   past PATH_MAX. Those files are still THERE; we just can't see them. To the
   diff they are indistinguishable from files the user deleted, so --delete
   would remove them from the peer. That is silent data loss from nothing worse
   than a chmod.

   Needs the server running. */
#include "harness.h"

#include <sys/stat.h>

#define ROOT_DIR "sandbox/source"
#define PEER_DIR "sandbox/received"

static char dirname_[64];
static char dir_path[256];
static char rel_file[128];
static char src_file[256];
static char peer_file[256];

static void cleanup(void)
{
    chmod(dir_path, 0755); /* always restore, or the tree is unremovable */
    unlink(src_file);
    unlink(peer_file);
    rmdir(dir_path);
}

static char *push(const char *flags, int *status)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "./bin/sync_push %s %s %s 2>&1", TEST_HOST, ROOT_DIR,
             flags);
    return run_capture(cmd, status);
}

int main(void)
{
    if (getuid() == 0) {
        printf("skipped: running as root, chmod 000 would not block us\n");
        return 0;
    }

    snprintf(dirname_, sizeof(dirname_), "_partial_%d", (int)getpid());
    snprintf(dir_path, sizeof(dir_path), "%s/%s", ROOT_DIR, dirname_);
    snprintf(rel_file, sizeof(rel_file), "%s/hidden.txt", dirname_);
    snprintf(src_file, sizeof(src_file), "%s/hidden.txt", dir_path);
    snprintf(peer_file, sizeof(peer_file), "%s/%s", PEER_DIR, rel_file);
    atexit(cleanup);

    mkdir_p(PEER_DIR);
    write_text_file(src_file, "still here\n");

    section("Partial source listing");

    /* --- get the file onto the peer while the directory is readable --- */
    int status = 0;
    free(push("", &status));
    CHECK(status == 0, "setup push failed (exit %d)", status);
    CHECK(access(peer_file, F_OK) == 0, "setup: %s never reached the peer", rel_file);
    ok("%s synced to the peer normally", rel_file);

    /* --- now hide it from the walk, exactly as a permissions slip would --- */
    CHECK(chmod(dir_path, 0000) == 0, "chmod: %s", strerror(errno));

    char *out = push("--delete", &status);
    CHECK(status != 0, "--delete SUCCEEDED on a partial listing (exit 0)");
    CHECK(strstr(out, "INCOMPLETE") != NULL, "no incompleteness warning printed");
    CHECK(strstr(out, "refusing to run --delete") != NULL,
          "did not refuse --delete; got:\n%s", out);
    free(out);
    ok("--delete refused while a directory was unreadable");

    /* --- the pass condition: the peer's copy is untouched --- */
    CHECK(access(peer_file, F_OK) == 0,
          "%s was DELETED from the peer — it still exists on the source!", rel_file);
    ok("the peer's copy survived");

    /* --- and a readable tree is still allowed to delete --- */
    CHECK(chmod(dir_path, 0755) == 0, "chmod back: %s", strerror(errno));
    out = push("--dry-run --delete", &status);
    CHECK(status == 0, "a readable tree was refused (exit %d)", status);
    CHECK(strstr(out, "INCOMPLETE") == NULL,
          "readable tree wrongly reported as incomplete");
    free(out);
    ok("a fully readable tree is not flagged, and --delete works again");

    done("an unreadable directory cannot turn into a delete \xf0\x9f\x94\x92");
    return 0;
}
