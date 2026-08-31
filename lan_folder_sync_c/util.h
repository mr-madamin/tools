/* Small helpers the standard C library doesn't give us: a growable byte
   buffer, a growable list of strings, and the path/dir plumbing that Python's
   os.path and os.makedirs hand over for free. */
#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

_Noreturn void die(const char *fmt, ...);
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

/* ---- strbuf: a growable, NUL-terminated byte buffer ---------------------- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} strbuf;

void sb_init(strbuf *sb);
void sb_free(strbuf *sb);
void sb_add(strbuf *sb, const void *data, size_t n);
void sb_addch(strbuf *sb, char c);
void sb_addstr(strbuf *sb, const char *s);
void sb_addf(strbuf *sb, const char *fmt, ...);
void sb_truncate(strbuf *sb, size_t len);
char *sb_detach(strbuf *sb, size_t *len); /* caller owns the returned buffer */

/* ---- strlist: a growable array of owned strings -------------------------- */

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} strlist;

void sl_init(strlist *sl);
void sl_push(strlist *sl, char *owned);
void sl_free(strlist *sl);

/* ---- paths --------------------------------------------------------------- */

char *path_join(const char *a, const char *b); /* os.path.join, roughly */
char *path_dirname(const char *path);          /* os.path.dirname */
int mkdir_p(const char *path);                 /* os.makedirs(exist_ok=True) */

int is_valid_utf8(const char *s, size_t len);

#endif
