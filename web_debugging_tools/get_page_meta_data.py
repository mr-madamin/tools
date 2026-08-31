#!/usr/bin/env python3

import sys
from html.parser import HTMLParser

# Colorize the label only when writing to a terminal, so piping/redirecting
# to a file stays plain text.
_USE_COLOR = sys.stdout.isatty()
_CYAN = "\033[36m"
_RESET = "\033[0m"


def emit(label, value):
    if _USE_COLOR:
        print(f"{_CYAN}{label}:{_RESET} {value}")
    else:
        print(f"{label}: {value}")


class MetaParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.in_title = False

    def process_meta(self, attrs):
        attrs = {k.lower(): v for k, v in attrs if k}

        name = attrs.get("name", "")
        prop = attrs.get("property", "")
        content = attrs.get("content", "")

        if name.lower() == "description":
            emit("description", content)

        og_key = prop or name
        if og_key.lower().startswith("og:"):
            emit(og_key, content)

    def process_link(self, attrs):
        attrs = {k.lower(): v for k, v in attrs if k}

        rel = attrs.get("rel", "").lower()
        href = attrs.get("href", "")
        hreflang = attrs.get("hreflang", "")

        rel_values = rel.split()

        if "canonical" in rel_values:
            emit("canonical", href)

        if "alternate" in rel_values:
            if hreflang:
                emit(f"alternate [{hreflang}]", href)
            else:
                emit("alternate", href)

    def handle_starttag(self, tag, attrs):
        tag = tag.lower()

        if tag == "title":
            self.in_title = True
        elif tag == "meta":
            self.process_meta(attrs)
        elif tag == "link":
            self.process_link(attrs)

    def handle_startendtag(self, tag, attrs):
        tag = tag.lower()

        if tag == "meta":
            self.process_meta(attrs)
        elif tag == "link":
            self.process_link(attrs)

    def handle_endtag(self, tag):
        if tag.lower() == "title":
            self.in_title = False

    def handle_data(self, data):
        if self.in_title:
            value = data.strip()
            if value:
                emit("title", value)


MetaParser().feed(sys.stdin.read())
