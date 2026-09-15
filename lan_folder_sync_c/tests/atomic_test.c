/* An aborted PUT must not damage the copy already on the receiver.

   Writing the destination directly means O_TRUNC destroys the good file the
   moment the transfer starts, so a peer that dies mid-body leaves a partial
   file where a complete one used to be. The receiver writes to a temp beside
   the target and rename()s it into place, so the old copy survives until the
   new one is whole.

   Needs the server running, like the other socket tests. */
#include "harness.h"

#include <dirent.h>
#include <sys/stat.h>

#define PEER_DIR  "sandbox/received"
#define GOOD_TEXT "THE ORIGINAL GOOD CONTENT\n"
#define CLAIMED   (5 * 1024 * 1024) /* announced */
#define DELIVERED 102400            /* actually sent before we vanish */

static char probe[64];
static char probe_path[256];

static void cleanup(void)
{
    unlink(probe_path);
}

static char *read_whole(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;

    strbuf sb;
    sb_init(&sb);
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        sb_add(&sb, chunk, n);
    fclose(f);
    return sb_detach(&sb, len);
}

/* Any leftover ".tmp" in the shared folder would show up in the manifest and
   get mirrored around as if it were a real file. */
static int stray_temp_files(void)
{
    DIR *d = opendir(PEER_DIR);
    if (d == NULL)
        return 0;

    int found = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (strstr(e->d_name, ".tmp") != NULL)
            found++;
    closedir(d);
    return found;
}

int main(void)
{
    snprintf(probe, sizeof(probe), "_atomic_probe_%d.txt", (int)getpid());
    snprintf(probe_path, sizeof(probe_path), "%s/%s", PEER_DIR, probe);
    atexit(cleanup);

    mkdir_p(PEER_DIR);
    write_text_file(probe_path, GOOD_TEXT);

    section("Aborted PUT leaves the old file alone");

    /* --- announce a big file, deliver a sliver, then drop the connection --- */
    int s = connect_authed();

    strbuf hdr;
    sb_init(&hdr);
    sb_addstr(&hdr, "{\"op\": \"PUT\", \"path\": ");
    json_escape(&hdr, probe);
    sb_addf(&hdr, ", \"size\": %d, \"mtime\": 1.0}", CLAIMED);
    CHECK(send_msg(s, hdr.data, hdr.len) == 0, "could not send the PUT header");
    sb_free(&hdr);

    char *sliver = xmalloc(DELIVERED);
    memset(sliver, 'X', DELIVERED);
    CHECK(send_all(s, sliver, DELIVERED) == 0, "could not send the partial body");
    free(sliver);

    close(s); /* vanish mid-body */
    usleep(300000);

    /* --- the file on disk must be exactly what it was --- */
    size_t len = 0;
    char *now = read_whole(probe_path, &len);
    CHECK(now != NULL, "%s is GONE after the aborted PUT", probe);
    CHECK(len == strlen(GOOD_TEXT),
          "%s is %zu bytes, was %zu — the aborted transfer overwrote it", probe,
          len, strlen(GOOD_TEXT));
    CHECK(memcmp(now, GOOD_TEXT, len) == 0, "%s survived but its content changed",
          probe);
    free(now);
    ok("the original %zu-byte file survived intact", strlen(GOOD_TEXT));

    CHECK(stray_temp_files() == 0,
          "an aborted PUT left a .tmp file behind in %s", PEER_DIR);
    ok("no stray .tmp left in the shared folder");

    /* --- a negative size must not blank the file either. It used to: the
       write loop never ran, and the empty temp was renamed over the original
       while the server logged "received" as if all was well. --- */
    {
        int s2 = connect_authed();
        strbuf h;
        sb_init(&h);
        sb_addstr(&h, "{\"op\": \"PUT\", \"path\": ");
        json_escape(&h, probe);
        sb_addstr(&h, ", \"size\": -1, \"mtime\": 0}");
        CHECK(send_msg(s2, h.data, h.len) == 0, "could not send the -1 PUT");
        sb_free(&h);

        json_value *reply = recv_json_frame(s2);
        close(s2);
        CHECK(reply != NULL, "server accepted a negative size without an ERROR");
        const char *rop = json_get_str(reply, "op");
        CHECK(rop != NULL && strcmp(rop, "ERROR") == 0,
              "expected ERROR for a negative size");
        json_free(reply);

        size_t l2 = 0;
        char *after = read_whole(probe_path, &l2);
        CHECK(after != NULL, "%s vanished after the negative-size PUT", probe);
        CHECK(l2 == strlen(GOOD_TEXT),
              "%s is %zu bytes, was %zu — a negative size truncated it", probe,
              l2, strlen(GOOD_TEXT));
        free(after);
        ok("a negative size was refused and left the file intact");
    }

    /* --- and the server is still serving --- */
    s = connect_authed();
    send_json(s, "{\"op\": \"MANIFEST\"}");
    json_value *msg = recv_json_frame(s);
    close(s);
    CHECK(msg != NULL, "server died after the aborted transfer");
    const char *op = json_get_str(msg, "op");
    CHECK(op != NULL && strcmp(op, "MANIFEST") == 0, "expected MANIFEST");
    json_free(msg);
    ok("server kept serving after the aborted transfer");

    done("an interrupted PUT cannot damage the file it was replacing \xf0\x9f\x92\xbe");
    return 0;
}
