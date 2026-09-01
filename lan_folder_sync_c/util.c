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
