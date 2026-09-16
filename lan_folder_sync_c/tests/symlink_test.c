/* A symlinked directory is skipped — deliberately, matching os.walk — but the
   caller has to be able to SAY so. Silently dropping it means a user who
   symlinks a folder into their sync directory watches nothing happen and is
   told nothing about why.

   Self-contained: build_manifest touches only the filesystem. */
#include "harness.h"

#include <sys/stat.h>

#define ROOT "sandbox/symlink_probe"

static char real_dir[256];
static char link_dir[256];
static char broken[256];
static char file_link[256];
static char plain[256];

static void cleanup(void)
{
    unlink(link_dir);
    unlink(broken);
    unlink(file_link);
    unlink(plain);
    char buried[512];
    snprintf(buried, sizeof(buried), "%s/buried.txt", real_dir);
    unlink(buried);
    rmdir(real_dir);
    rmdir(ROOT);
}

static int listed(const strlist *sl, const char *name)
{
    for (size_t i = 0; i < sl->count; i++)
        if (strcmp(sl->items[i], name) == 0)
            return 1;
    return 0;
}

int main(void)
{
    snprintf(real_dir, sizeof(real_dir), "sandbox/symlink_target_%d", (int)getpid());
    snprintf(link_dir, sizeof(link_dir), "%s/linked_dir", ROOT);
    snprintf(broken, sizeof(broken), "%s/broken_link", ROOT);
    snprintf(file_link, sizeof(file_link), "%s/linked_file.txt", ROOT);
    snprintf(plain, sizeof(plain), "%s/plain.txt", ROOT);
    atexit(cleanup);

    mkdir_p(real_dir);
    char buried[512];
    snprintf(buried, sizeof(buried), "%s/buried.txt", real_dir);
    write_text_file(buried, "you cannot see me\n");
    write_text_file(plain, "ordinary\n");

    CHECK(symlink(real_dir, link_dir) == 0, "symlink dir: %s", strerror(errno));
    CHECK(symlink("/nonexistent/nowhere", broken) == 0, "symlink broken: %s",
          strerror(errno));
    CHECK(symlink("plain.txt", file_link) == 0, "symlink file: %s", strerror(errno));

    section("Symlinks in the manifest");

    manifest m;
    CHECK(build_manifest(ROOT, &m) == 0, "build_manifest reported a partial walk");

    /* A symlink to a FILE is followed — os.walk stats it through the link. */
    int has_plain = 0, has_file_link = 0, has_buried = 0;
    for (size_t i = 0; i < m.count; i++) {
        if (strcmp(m.items[i].path, "plain.txt") == 0)
            has_plain = 1;
        if (strcmp(m.items[i].path, "linked_file.txt") == 0)
            has_file_link = 1;
        if (strstr(m.items[i].path, "buried.txt") != NULL)
            has_buried = 1;
    }
    CHECK(has_plain, "the ordinary file is missing from the manifest");
    CHECK(has_file_link, "a symlink to a FILE should still be synced");
    ok("ordinary file and symlinked file both listed");

    CHECK(!has_buried, "walk followed a symlinked directory — it must not");
    ok("the symlinked directory's contents stayed out of the manifest");

    /* ...and the skip is reported rather than silent. */
    CHECK(m.skipped_links.count == 2,
          "expected 2 skipped symlinks, got %zu", m.skipped_links.count);
    CHECK(listed(&m.skipped_links, "linked_dir"),
          "the symlinked directory was skipped without being reported");
    CHECK(listed(&m.skipped_links, "broken_link"),
          "the broken link was skipped without being reported");
    ok("both skipped symlinks are reported by name");

    manifest_free(&m);

    /* A tree with no symlinks must not report any. */
    manifest clean;
    CHECK(build_manifest(real_dir, &clean) == 0, "partial walk on a plain tree");
    CHECK(clean.skipped_links.count == 0,
          "a tree with no symlinks reported %zu skipped", clean.skipped_links.count);
    manifest_free(&clean);
    ok("a tree without symlinks reports none");

    done("symlinked directories are skipped, and never in silence \xf0\x9f\x94\x97");
    return 0;
}
