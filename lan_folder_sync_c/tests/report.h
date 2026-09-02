/* Shared pretty-printer for the manual test runners — the C twin of
   tests/_report.py. A section header per group, a green check per passing
   assertion, dim context lines for captured subprocess output, and a tally.
   Colour only on a TTY, so piped logs stay clean. */
#ifndef REPORT_H
#define REPORT_H

void section(const char *title);
void ok(const char *fmt, ...);
void info(const char *text);
void done(const char *title);
_Noreturn void fail(const char *fmt, ...);

/* The runner's assert: on failure it prints the red verdict and exits 1.
   Python got this from `assert` plus an excepthook; here it's a macro. */
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond))                                                           \
            fail(__VA_ARGS__);                                                 \
    } while (0)

#endif
