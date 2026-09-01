#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

_Noreturn void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (p == NULL)
        die("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (q == NULL)
        die("out of memory");
    return q;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *copy = xmalloc(n);
    memcpy(copy, s, n);
    return copy;
}

/* ---- strbuf --------------------------------------------------------------- */

void sb_init(strbuf *sb)
{
    sb->data = xmalloc(64);
    sb->data[0] = '\0';
    sb->len = 0;
    sb->cap = 64;
}

void sb_free(strbuf *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

static void sb_grow(strbuf *sb, size_t extra)
{
    if (sb->len + extra + 1 <= sb->cap)
        return;
    while (sb->cap < sb->len + extra + 1)
        sb->cap *= 2;
    sb->data = xrealloc(sb->data, sb->cap);
}

void sb_add(strbuf *sb, const void *data, size_t n)
{
    sb_grow(sb, n);
    memcpy(sb->data + sb->len, data, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_addch(strbuf *sb, char c) { sb_add(sb, &c, 1); }

void sb_addstr(strbuf *sb, const char *s) { sb_add(sb, s, strlen(s)); }

void sb_addf(strbuf *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        die("vsnprintf failed");

    sb_grow(sb, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb->len += (size_t)n;
}

void sb_truncate(strbuf *sb, size_t len)
{
    if (len < sb->len)
    {
        sb->len = len;
        sb->data[len] = '\0';
    }
}

char *sb_detach(strbuf *sb, size_t *len)
{
    char *data = sb->data;
    if (len != NULL)
        *len = sb->len;
    sb->data = NULL;
    sb->len = sb->cap = 0;
    return data;
}

/* ---- strlist -------------------------------------------------------------- */

void sl_init(strlist *sl)
{
    sl->items = NULL;
    sl->count = sl->cap = 0;
}

void sl_push(strlist *sl, char *owned)
{
    if (sl->count == sl->cap)
    {
        sl->cap = sl->cap ? sl->cap * 2 : 16;
        sl->items = xrealloc(sl->items, sl->cap * sizeof(*sl->items));
    }
    sl->items[sl->count++] = owned;
}

void sl_free(strlist *sl)
{
    for (size_t i = 0; i < sl->count; i++)
        free(sl->items[i]);
    free(sl->items);
    sl->items = NULL;
    sl->count = sl->cap = 0;
}

/* ---- paths ---------------------------------------------------------------- */

char *path_join(const char *a, const char *b)
{
    if (b[0] == '/' || a[0] == '\0')
        return xstrdup(b);

    strbuf sb;
    sb_init(&sb);
    sb_addstr(&sb, a);
    if (sb.len > 0 && sb.data[sb.len - 1] != '/')
        sb_addch(&sb, '/');
    sb_addstr(&sb, b);
    return sb_detach(&sb, NULL);
}

char *path_dirname(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (slash == NULL)
        return xstrdup(".");
    if (slash == path)
        return xstrdup("/");

    size_t n = (size_t)(slash - path);
    char *dir = xmalloc(n + 1);
    memcpy(dir, path, n);
    dir[n] = '\0';
    return dir;
}

int mkdir_p(const char *path)
{
    char *work = xstrdup(path);
    int rc = 0;

    for (char *p = work + 1; *p != '\0'; p++)
    {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(work, 0777) != 0 && errno != EEXIST)
        {
            rc = -1;
            break;
        }
        *p = '/';
    }
    if (rc == 0 && mkdir(work, 0777) != 0 && errno != EEXIST)
        rc = -1;

    free(work);
    return rc;
}

/* Python decodes every header with .decode("utf-8") and rejects what doesn't
   fit; we have to check by hand to refuse the same frames. */
int is_valid_utf8(const char *s, size_t len)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;

    while (i < len)
    {
        unsigned char c = p[i];
        size_t extra;
        unsigned int cp;

        if (c < 0x80)
        {
            i++;
            continue;
        }
        else if ((c & 0xE0) == 0xC0)
        {
            extra = 1;
            cp = c & 0x1Fu;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            extra = 2;
            cp = c & 0x0Fu;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            extra = 3;
            cp = c & 0x07u;
        }
        else
            return 0;

        if (i + extra >= len)
            return 0;
        for (size_t k = 1; k <= extra; k++)
        {
            if ((p[i + k] & 0xC0) != 0x80)
                return 0;
            cp = (cp << 6) | (p[i + k] & 0x3Fu);
        }
        if (extra == 1 && cp < 0x80)
            return 0; /* overlong */
        if (extra == 2 && cp < 0x800)
            return 0;
        if (extra == 3 && cp < 0x10000)
            return 0;
        if (cp > 0x10FFFF)
            return 0;
        if (cp >= 0xD800 && cp <= 0xDFFF)
            return 0; /* lone surrogate */
        i += extra + 1;
    }
    return 1;
}
