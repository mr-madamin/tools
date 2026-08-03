#!/usr/bin/env python3

import sys
from html.parser import HTMLParser


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
            print("description:", content)

        og_key = prop or name
        if og_key.lower().startswith("og:"):
            print(f"{og_key}: {content}")

    def process_link(self, attrs):
        attrs = {k.lower(): v for k, v in attrs if k}

        rel = attrs.get("rel", "").lower()
        href = attrs.get("href", "")
        hreflang = attrs.get("hreflang", "")

        rel_values = rel.split()

        if "canonical" in rel_values:
            print("canonical:", href)

        if "alternate" in rel_values:
            if hreflang:
                print(f"alternate [{hreflang}]: {href}")
            else:
                print("alternate:", href)

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
                print("title:", value)


MetaParser().feed(sys.stdin.read())
