# web_debugging_tools

Small command-line helpers for inspecting web pages.

## get_page_meta_data.py

Reads HTML from **stdin** and prints out SEO/social metadata:

- `title`
- `description`
- **All** Open Graph tags (`og:*`) — including nested ones like
  `og:image:width`, `og:image:type`, `og:locale`, `og:site_name`, etc.
- `canonical` link
- **All** `alternate` links (with `hreflang` when present)

It does not fetch URLs itself — you pipe HTML into it.

When printing to a terminal, the tag label (the part before the colon) is
colorized for readability. When the output is piped or redirected to a file,
it stays plain text.

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

A page can emit many tags — every `og:*` and every `alternate` link is
printed. For example:

```
title: Example Domain
description: Example description from the meta tag
og:title: Example Domain
og:description: A longer social description
og:locale: en_US
og:site_name: Example
og:type: website
og:image: https://example.com/og.png
og:image:width: 1200
og:image:height: 630
og:image:type: image/png
canonical: https://example.com/
alternate [en-US]: https://example.com/
alternate [ru-RU]: https://example.ru/
alternate [kk-KZ]: https://example.kz/kk/
```