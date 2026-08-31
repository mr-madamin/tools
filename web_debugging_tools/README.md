# web_debugging_tools

Small command-line helpers for inspecting web pages.

## get_page_meta_data.py

Reads HTML from **stdin** and prints out SEO/social metadata:

- `title`
- `description`
- Open Graph tags (`og:*`)
- `canonical` link
- `alternate` links (with `hreflang` when present)

It does not fetch URLs itself — you pipe HTML into it.

### Direct usage

```sh
curl -sL https://example.com | python3 web_debugging_tools/get_page_meta_data.py
```

`curl -sL` follows redirects (`-L`) and stays quiet (`-s`).

You can also read from a local file:

```sh
python3 web_debugging_tools/get_page_meta_data.py < page.html
```

### Shell function (recommended)

Add a wrapper to your `~/.zshrc` so you can pass a URL directly:

```sh
pagemeta() {
  curl -sL "$1" | python3 ~/Desktop/projects/tools/web_debugging_tools/get_page_meta_data.py
}
```

Reload and use it:

```sh
source ~/.zshrc
pagemeta https://example.com
```

### Running the script directly

The script has a `#!/usr/bin/env python3` shebang, so you can make it
executable and drop the explicit `python3`:

```sh
chmod +x ~/Desktop/projects/tools/web_debugging_tools/get_page_meta_data.py

pagemeta() {
  curl -sL "$1" | ~/Desktop/projects/tools/web_debugging_tools/get_page_meta_data.py
}
```

### Example output

```
title: Example Domain
description: Example description from the meta tag
og:title: Example Domain
og:type: website
canonical: https://example.com/
alternate [en]: https://example.com/en
```