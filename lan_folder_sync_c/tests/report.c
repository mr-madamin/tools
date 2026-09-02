#include "report.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int passed = 0;

static int tty(void) { return isatty(STDOUT_FILENO); }

static void colour(const char *code, const char *text)
{
    if (tty())
        printf("\033[%sm%s\033[0m", code, text);
    else
        fputs(text, stdout);
}

void section(const char *title)
{
    char line[256];
    int n = snprintf(line, sizeof(line), "\xe2\x94\x80\xe2\x94\x80 %s ", title);

    /* Python's ljust(45, "─") pads to 45 *characters*; our dashes are 3 bytes
       each, so count code points (skip UTF-8 continuation bytes) instead. */
    int width = 4; /* "── " plus the space after the title */
    for (const unsigned char *p = (const unsigned char *)title; *p != '\0'; p++)
        if ((*p & 0xC0) != 0x80)
            width++;

    for (int i = width; i < 45 && n < (int)sizeof(line) - 4; i++)
        n += snprintf(line + n, sizeof(line) - (size_t)n, "\xe2\x94\x80");

    printf("\n");
    colour("1;36", line);
    printf("\n");
}

void ok(const char *fmt, ...)
{
    char msg[1024], out[1088];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    passed++;
    snprintf(out, sizeof(out), "\xe2\x9c\x85 %s", msg);
    printf("  ");
    colour("32", out);
    printf("\n");
}

void info(const char *text)
{
    char *copy = strdup(text);
    if (copy == NULL)
        return;

    for (char *line = strtok(copy, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        char out[1088];
        snprintf(out, sizeof(out), "\xc2\xb7 %s", line);
        printf("  ");
        colour("2", out);
        printf("\n");
    }
    free(copy);
}

void done(const char *title)
{
    char out[1088];
    snprintf(out, sizeof(out), "\n\xf0\x9f\x8e\x89 %d checks \xe2\x80\x94 %s\n",
             passed, title);
    colour("1;32", out);
    printf("\n");
}

_Noreturn void fail(const char *fmt, ...)
{
    char msg[1024], out[1152];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    snprintf(out, sizeof(out), "\n\xe2\x9d\x8c FAILED after %d passing check(s) \xe2\x80\x94 %s\n",
             passed, msg);
    colour("1;31", out);
    printf("\n");
    exit(1);
}
