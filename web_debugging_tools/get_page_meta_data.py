#!/usr/bin/env python3

import sys
from html.parser import HTMLParser


class P(HTMLParser):
    def __init__(self):
        super().__init__()
        self.in_title = False

    def process_meta(self, attrs):
        attrs = {k.lower(): v for k, v in attrs if k}

        name = attrs.get("name", "")
        prop = attrs.get("property", "")
        content = attrs.get("content", "")

        if name == "description":
            print("description:", content)

        og_key = prop or name
        if og_key.startswith("og:"):
            print(f"{og_key}: {content}")

    def handle_starttag(self, tag, attrs):
        tag = tag.lower()
        if tag == "title":
            self.in_title = True
        elif tag == "meta":
            self.process_meta(attrs)

    def handle_startendtag(self, tag, attrs):
        if tag.lower() == "meta":
            self.process_meta(attrs)

    def handle_endtag(self, tag):
        if tag.lower() == "title":
            self.in_title = False

    def handle_data(self, data):
        if self.in_title:
            title = data.strip()
            if title:
                print("title:", title)


P().feed(sys.stdin.read())
