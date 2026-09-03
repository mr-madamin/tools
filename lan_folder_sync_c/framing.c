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

/* ---- files ---------------------------------------------------------------- */

static double stat_mtime(const struct stat *st)
{
#if defined(__APPLE__)
    return (double)st->st_mtimespec.tv_sec + st->st_mtimespec.tv_nsec / 1e9;
#else
    return (double)st->st_mtim.tv_sec + st->st_mtim.tv_nsec / 1e9;
#endif
}

int send_file(int fd, const char *root_dir, const char *rel_path)
{
    char *full_path = path_join(root_dir, rel_path);

    struct stat st;
    if (stat(full_path, &st) != 0)
    {
        free(full_path);
        return -1;
    }

    strbuf meta;
    sb_init(&meta);
    sb_addstr(&meta, "{\"op\": \"PUT\", \"path\": ");
    json_escape(&meta, rel_path);
    sb_addf(&meta, ", \"size\": %lld, \"mtime\": %.6f}", (long long)st.st_size,
            stat_mtime(&st));

    int rc = send_msg(fd, meta.data, meta.len);
    sb_free(&meta);
    if (rc != 0)
    {
        free(full_path);
        return -1;
    }

    int src = open(full_path, O_RDONLY);
    free(full_path);
    if (src < 0)
        return -1;

    char buf[CHUNK];
    for (;;)
    {
        ssize_t n = read(src, buf, sizeof(buf));
        if (n == 0)
            break;
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            close(src);
            return -1;
        }
        if (send_all(fd, buf, (size_t)n) != 0)
        {
            close(src);
            return -1;
        }
    }
    close(src);
    return 0;
}

int send_delete(int fd, const char *rel_path)
{
    strbuf sb;
    sb_init(&sb);
    sb_addstr(&sb, "{\"op\": \"DELETE\", \"path\": ");
    json_escape(&sb, rel_path);
    sb_addch(&sb, '}');
    int rc = send_msg(fd, sb.data, sb.len);
    sb_free(&sb);
    return rc;
}

/* A cheap first pass: no absolute paths, no ".." component. Runs before we
   create any directories, so a traversal never gets to mkdir anything. */
int path_is_lexically_safe(const char *rel_path)
{
    if (rel_path[0] == '\0' || rel_path[0] == '/')
        return 0;

    const char *p = rel_path;
    while (*p != '\0')
    {
        const char *slash = strchr(p, '/');
        size_t n = slash != NULL ? (size_t)(slash - p) : strlen(p);
        if (n == 2 && p[0] == '.' && p[1] == '.')
            return 0;
        if (slash == NULL)
            break;
        p = slash + 1;
    }
    return 1;
}

/* Resolve `rel_path` under `base` and refuse anything that lands outside it.
   ".." and absolute paths are already gone by the time we get here, so the one
   remaining escape is a symlink: walk the path a component at a time and, each
   time a component *is* a link, resolve it and re-check containment. Unlike
   realpath() on the whole thing, this works for a path that doesn't exist yet
   — which is the normal case for a PUT, and for an idempotent DELETE. */
char *safe_path(const char *base, const char *rel_path)
{
    char base_real[PATH_MAX];
    if (realpath(base, base_real) == NULL)
        return NULL;
    if (!path_is_lexically_safe(rel_path))
        return NULL;

    size_t base_len = strlen(base_real);
    char *cur = xstrdup(base_real);
    char *rest = xstrdup(rel_path);

    for (char *comp = strtok(rest, "/"); comp != NULL; comp = strtok(NULL, "/"))
    {
        if (comp[0] == '\0' || strcmp(comp, ".") == 0)
            continue;

        char *next = path_join(cur, comp);
        free(cur);

        struct stat lst;
        if (lstat(next, &lst) == 0 && S_ISLNK(lst.st_mode))
        {
            char resolved[PATH_MAX];
            if (realpath(next, resolved) == NULL)
            { /* broken link — refuse it */
                free(next);
                free(rest);
                return NULL;
            }
            free(next);
            next = xstrdup(resolved);
        }

        if (strncmp(next, base_real, base_len) != 0 ||
            (next[base_len] != '/' && next[base_len] != '\0'))
        {
            free(next);
            free(rest);
            return NULL;
        }
        cur = next;
    }

    free(rest);
    if (strcmp(cur, base_real) == 0)
    { /* the path named the root itself */
        free(cur);
        return NULL;
    }
    return cur;
}
