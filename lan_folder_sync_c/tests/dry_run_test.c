/* --dry-run prints the plan and transfers nothing. Mirrors dry_run_test.py. */
#include "harness.h"

#define ROOT_DIR "sandbox/source"

int main(void)
{
    char probe[64], probe_path[256];
    snprintf(probe, sizeof(probe), "_dryrun_probe_%d.txt", (int)getpid());
    snprintf(probe_path, sizeof(probe_path), "%s/%s", ROOT_DIR, probe);

    section("Dry-run");

    /* 1. Plant a file locally that the peer definitely does not have */
    write_text_file(probe_path, "dry-run probe - must never reach the peer\n");

    /* 2. Sanity: the peer starts without it */
    manifest before;
    remote_manifest(&before);
    CHECK(!manifest_has(&before, probe), "peer already has %s?!", probe);
    manifest_free(&before);

    /* 3. Run the real push, in dry-run, and capture what it prints */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "./bin/sync_push %s %s --dry-run 2>&1", TEST_HOST,
             ROOT_DIR);
    int status = 0;
    char *out = run_capture(cmd, &status);
    info(out);
    CHECK(status == 0, "push crashed (exit %d)", status);

    /* 4. It should plan to send the probe... */
    CHECK(strstr(out, "DRY RUN") != NULL, "no DRY RUN banner in output");
    CHECK(strstr(out, probe) != NULL, "%s not listed in the plan", probe);

    /* 5. ...but the peer must still not have it. The plan was talk, not action */
    manifest after;
    remote_manifest(&after);
    CHECK(!manifest_has(&after, probe),
          "%s reached the peer - dry-run transferred a file!", probe);
    manifest_free(&after);

    ok("dry-run planned PUT %s but transferred nothing", probe);
    free(out);
    unlink(probe_path);

    done("dry-run previews without touching the peer");
    return 0;
}
