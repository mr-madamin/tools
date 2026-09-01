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
