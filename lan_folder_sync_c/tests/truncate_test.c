/* A file that changes size mid-send must not desynchronise the stream.

   The body is the only unframed part of the protocol: the receiver finds the
   next frame by counting exactly `size` bytes. If the sender declares one
   length and writes another, the receiver reads the following header as file
   content and every later file in the session is silently corrupt.

   Self-contained — needs no server. A socketpair gives us the backpressure to
   make the race deterministic: send_file blocks once the buffer fills, and we
   truncate the file while it is parked there. */
#include "harness.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>

#define ROOT     "sandbox"
#define BIG      (5 * 1024 * 1024) /* comfortably larger than any socket buffer */
#define SHRUNK   4096

static char rel[64];
static char full[256];
static int sv[2];
static int worker_rc;

/* Send the file, then a sentinel frame. The sentinel is the real assertion:
   it is only readable as a frame if the body was exactly as long as declared. */
static void *worker(void *unused)
{
    (void)unused;
    worker_rc = send_file(sv[0], ROOT, rel);
    send_json(sv[0], "{\"op\": \"SENTINEL\"}");
    return NULL;
}

static void make_big_file(void)
{
    int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    CHECK(fd >= 0, "cannot create %s: %s", full, strerror(errno));

    char chunk[65536];
    memset(chunk, 'A', sizeof(chunk));
    for (size_t written = 0; written < BIG; written += sizeof(chunk))
        CHECK(write(fd, chunk, sizeof(chunk)) == (ssize_t)sizeof(chunk),
              "short write building the test file");
    close(fd);
}

int main(void)
{
    snprintf(rel, sizeof(rel), "_truncate_probe_%d.bin", (int)getpid());
    snprintf(full, sizeof(full), "%s/%s", ROOT, rel);

    mkdir_p(ROOT);
    make_big_file();

    section("File truncated mid-send");

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair: %s",
          strerror(errno));

    pthread_t t;
    CHECK(pthread_create(&t, NULL, worker, NULL) == 0, "pthread_create failed");

    /* Let the sender fill the socket buffer and park in send_all(), then pull
       the file out from under it. */
    usleep(300000);
    CHECK(truncate(full, SHRUNK) == 0, "truncate: %s", strerror(errno));

    /* --- the header tells us how many body bytes to expect --- */
    char *payload = NULL;
    size_t len = 0;
    CHECK(recv_msg(sv[1], &payload, &len) == FRAME_OK, "no PUT header arrived");

    json_value *header = json_parse(payload, len);
    free(payload);
    CHECK(header != NULL, "PUT header did not parse");

    double declared = 0;
    CHECK(json_get_num(header, "size", &declared), "PUT header has no size");
    json_free(header);
    CHECK((long long)declared == BIG, "declared %lld, expected %d",
          (long long)declared, BIG);
    ok("header declared %lld bytes", (long long)declared);

    /* --- drain exactly that many body bytes --- */
    long long body = 0;
    char buf[65536];
    while (body < (long long)declared) {
        size_t want = (size_t)((long long)declared - body);
        if (want > sizeof(buf))
            want = sizeof(buf);
        CHECK(recv_exactly(sv[1], buf, want) == FRAME_OK,
              "body ended early at %lld of %lld bytes — the sender wrote fewer "
              "bytes than it promised",
              body, (long long)declared);
        body += want;
    }
    ok("body delivered all %lld bytes despite the truncation", body);

    /* --- the pass condition: the next frame is still a frame --- */
    CHECK(recv_msg(sv[1], &payload, &len) == FRAME_OK,
          "no sentinel frame — the stream desynchronised");
    json_value *sentinel = json_parse(payload, len);
    free(payload);
    CHECK(sentinel != NULL, "sentinel did not parse — stream desynchronised");
    const char *op = json_get_str(sentinel, "op");
    CHECK(op != NULL && strcmp(op, "SENTINEL") == 0,
          "expected SENTINEL, got '%s' — stream desynchronised",
          op != NULL ? op : "(none)");
    json_free(sentinel);
    ok("the following frame arrived intact — stream still in sync");

    pthread_join(t, NULL);
    CHECK(worker_rc == SEND_CHANGED, "send_file returned %d, expected "
          "SEND_CHANGED (%d) to report the file changed", worker_rc, SEND_CHANGED);
    ok("send_file reported SEND_CHANGED instead of failing the session");

    close(sv[0]);
    close(sv[1]);
    unlink(full);

    done("a file truncated mid-send cannot corrupt the frames after it \xe2\x9c\x82\xef\xb8\x8f");
    return 0;
}
