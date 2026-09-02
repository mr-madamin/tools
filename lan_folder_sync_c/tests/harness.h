/* Bits every runner in this folder needs: dial loopback, do the HELLO, send a
   hand-built frame, read one back. Header-only so the Makefile keeps its
   one-object-per-test rule. */
#ifndef HARNESS_H
#define HARNESS_H

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.h"
#include "framing.h"
#include "json.h"
#include "report.h"
#include "util.h"

#define TEST_HOST "127.0.0.1"
#define TEST_PORT 8765

static inline char *test_token(void)
{
    static char *cached = NULL;
    if (cached == NULL) {
        config *cfg = load_config(NULL);
        cached = xstrdup(cfg->token);
        config_free(cfg);
    }
    return cached;
}

static inline int dial_test_server(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s >= 0, "socket: %s", strerror(errno));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT);
    inet_pton(AF_INET, TEST_HOST, &addr.sin_addr);

    CHECK(connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0,
          "no server on %s:%d (%s) — start bin/sync_server in another terminal",
          TEST_HOST, TEST_PORT, strerror(errno));
    return s;
}

/* Dial and authenticate; the returned socket is ready for real work. */
static inline int connect_authed(void)
{
    int s = dial_test_server();
    json_value *reply = NULL;
    CHECK(handshake(s, test_token(), &reply) == FRAME_OK, "handshake: peer closed");
    const char *op = json_str(json_get(reply, "op"));
    CHECK(op != NULL && strcmp(op, "OK") == 0, "handshake failed");
    json_free(reply);
    return s;
}

/* Read one framed reply and parse it. Returns NULL on EOF (the caller decides
   whether that's the pass condition or the failure). */
static inline json_value *recv_json_frame(int s)
{
    char *payload = NULL;
    size_t len = 0;
    if (recv_msg(s, &payload, &len) != FRAME_OK)
        return NULL;
    json_value *v = json_parse(payload, len);
    free(payload);
    return v;
}

/* Ask the live server what it currently holds. */
static inline void remote_manifest(manifest *out)
{
    int s = connect_authed();
    send_json(s, "{\"op\": \"MANIFEST\"}");
    json_value *msg = recv_json_frame(s);
    close(s);

    CHECK(msg != NULL, "server closed without sending a manifest");
    CHECK(manifest_from_json(json_get(msg, "files"), out) == 0,
          "manifest reply had no \"files\"");
    json_free(msg);
}

static inline int manifest_has(const manifest *m, const char *path)
{
    for (size_t i = 0; i < m->count; i++)
        if (strcmp(m->items[i].path, path) == 0)
            return 1;
    return 0;
}

/* Run a command, capture stdout+stderr, return the exit status. */
static inline char *run_capture(const char *cmd, int *status)
{
    FILE *p = popen(cmd, "r");
    CHECK(p != NULL, "popen(%s): %s", cmd, strerror(errno));

    strbuf sb;
    sb_init(&sb);
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), p)) > 0)
        sb_add(&sb, chunk, n);

    int rc = pclose(p);
    *status = (rc == -1) ? -1 : (rc >> 8) & 0xFF;
    return sb_detach(&sb, NULL);
}

static inline void write_text_file(const char *path, const char *text)
{
    char *dir = path_dirname(path);
    mkdir_p(dir);
    free(dir);

    FILE *f = fopen(path, "w");
    CHECK(f != NULL, "cannot write %s: %s", path, strerror(errno));
    fputs(text, f);
    fclose(f);
}

#endif
