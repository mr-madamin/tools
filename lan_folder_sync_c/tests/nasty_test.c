/* Framing boundaries, self-contained — no server needed. Mirrors nasty_test.py. */
#include <pthread.h>
#include <sys/socket.h>

#include "harness.h"

#define BIG_LEN 1000000

static int reader_fd;
static char *reader_payload;
static size_t reader_len;
static int reader_rc;

static void *reader_main(void *unused)
{
    reader_rc = recv_msg(reader_fd, &reader_payload, &reader_len);
    return NULL;
}

int main(void)
{
    section("Framing boundaries \xe2\x80\x94 self-contained, no server");

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair: %s",
          strerror(errno));
    int a = sv[0], b = sv[1];

    /* --- Test 1: payload far bigger than the socket buffer (partial reads) ---
       send_msg does one big write; on a socketpair that blocks once the kernel
       buffer fills, so a reader thread must drain concurrently. recv_exactly on
       the reader side then loops over many partial recv()s to reassemble 1 MB. */
    char *big = xmalloc(BIG_LEN);
    memset(big, 'A', BIG_LEN);

    reader_fd = b;
    pthread_t reader;
    CHECK(pthread_create(&reader, NULL, reader_main, NULL) == 0, "pthread_create");

    CHECK(send_msg(a, big, BIG_LEN) == 0, "send_msg failed"); /* blocks/unblocks as
                                              the reader drains — that's the point */
    pthread_join(reader, NULL);

    CHECK(reader_rc == FRAME_OK, "reader hit EOF instead of a frame");
    CHECK(reader_len == BIG_LEN, "MISMATCH: sent %d, got %zu", BIG_LEN, reader_len);
    CHECK(memcmp(reader_payload, big, BIG_LEN) == 0, "1 MB payload came back altered");
    ok("big payload: %d bytes survived the round trip intact", BIG_LEN);
    free(reader_payload);
    free(big);

    /* --- Test 2: two frames shoved into ONE write (coalesced on the wire) ---
       Bypass send_msg on purpose so both frames land back-to-back in the
       receiver's buffer. recv_msg must split them at the right boundary. */
    unsigned char frames[] = { 0, 0, 0, 5, 'h', 'e', 'l', 'l', 'o',
                               0, 0, 0, 3, 'b', 'y', 'e' };
    CHECK(send_all(a, frames, sizeof(frames)) == 0, "coalesced write failed");

    char *r1 = NULL, *r2 = NULL;
    size_t l1 = 0, l2 = 0;
    CHECK(recv_msg(b, &r1, &l1) == FRAME_OK, "frame 1: EOF");
    CHECK(recv_msg(b, &r2, &l2) == FRAME_OK, "frame 2: EOF");
    CHECK(l1 == 5 && memcmp(r1, "hello", 5) == 0, "frame 1 wrong: '%s'", r1);
    CHECK(l2 == 3 && memcmp(r2, "bye", 3) == 0, "frame 2 wrong: '%s'", r2);
    ok("coalesced: split cleanly into '%s' then '%s'", r1, r2);
    free(r1);
    free(r2);

    close(a);
    close(b);
    done("all boundaries held \xe2\x80\x94 framing works");
    return 0;
}
