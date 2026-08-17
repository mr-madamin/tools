"""Shared pretty-printer for the manual test runners.

Keeps output easy to scan: a section header per group, a green ✅ per passing
check, dim context lines for captured subprocess output, and a final tally.
Works both as `python3 -m tests.<name>` and under pytest:

    from tests._report import section, ok, info, done

Colour is emitted only when stdout is a TTY, so piped/captured logs stay clean
(the ✅ still shows — it's just text).
"""

import sys

_TTY = sys.stdout.isatty()
_passed = 0


def _c(code, text):
    """Wrap text in an ANSI colour, but only on a real terminal."""
    return f"\033[{code}m{text}\033[0m" if _TTY else text


def section(title):
    """Header before a group of related checks."""
    line = f"── {title} ".ljust(45, "─")  # pad the plain text, THEN colour
    print("\n" + _c("1;36", line))


def ok(msg):
    """Record and print one passing check."""
    global _passed
    _passed += 1
    print("  " + _c("32", f"✅ {msg}"))


def info(msg):
    """Dim context line(s) — e.g. captured subprocess output."""
    for line in str(msg).rstrip("\n").splitlines():
        print("  " + _c("2", f"· {line}"))


def done(title="all checks passed"):
    """Final tally banner."""
    print(_c("1;32", f"\n🎉 {_passed} checks — {title}\n"))
