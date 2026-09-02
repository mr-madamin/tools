#include "framing.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define CHUNK 65536

/* ---- byte plumbing -------------------------------------------------------- */

/* Read exactly n bytes, or report that the peer closed first. The loop is the
   whole point: one recv() can return short, and regularly does. */
int recv_exactly(int fd, void *buf, size_t n)
{
    unsigned char *out = buf;
    size_t got = 0;

    while (got < n)
    {
        ssize_t r = recv(fd, out + got, n - got, 0);
        if (r == 0)
            return FRAME_EOF;
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return FRAME_ERR;
        }
        got += (size_t)r;
    }
    return FRAME_OK;
}

int send_all(int fd, const void *buf, size_t n)
{
    const unsigned char *p = buf;
    size_t sent = 0;

    while (sent < n)
    {
        ssize_t w = send(fd, p + sent, n - sent, 0);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)w;
    }
    return 0;
}

/* ---- framing -------------------------------------------------------------- */

int send_msg(int fd, const void *payload, size_t len)
{
    unsigned char header[4];
    header[0] = (unsigned char)((len >> 24) & 0xFF);
    header[1] = (unsigned char)((len >> 16) & 0xFF);
    header[2] = (unsigned char)((len >> 8) & 0xFF);
    header[3] = (unsigned char)(len & 0xFF);

    /* One buffer, one write — Python's sendall(header + payload) does the same,
       and it keeps the length prefix from ever landing without its body. */
    strbuf sb;
    sb_init(&sb);
    sb_add(&sb, header, 4);
    sb_add(&sb, payload, len);
    int rc = send_all(fd, sb.data, sb.len);
    sb_free(&sb);
    return rc;
}

int send_json(int fd, const char *json)
{
    return send_msg(fd, json, strlen(json));
}

int send_error(int fd, const char *message)
{
    strbuf sb;
    sb_init(&sb);
    sb_addstr(&sb, "{\"op\": \"ERROR\", \"message\": ");
    json_escape(&sb, message);
    sb_addch(&sb, '}');
    int rc = send_msg(fd, sb.data, sb.len);
    sb_free(&sb);
    return rc;
}

int recv_msg(int fd, char **out, size_t *out_len)
{
    unsigned char header[4];
    int rc = recv_exactly(fd, header, 4);
    if (rc != FRAME_OK)
        return rc;

    size_t len = ((size_t)header[0] << 24) | ((size_t)header[1] << 16) |
                 ((size_t)header[2] << 8) | (size_t)header[3];

    char *buf = xmalloc(len + 1);
    if (len > 0)
    {
        rc = recv_exactly(fd, buf, len);
        if (rc != FRAME_OK)
        {
            free(buf);
            return rc;
        }
    }
    buf[len] = '\0';

    *out = buf;
    if (out_len != NULL)
        *out_len = len;
    return FRAME_OK;
}
