#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""tests/netsurf/mirror.py: save a page and its scripts and style sheets for offline runs.

    tests/netsurf/mirror.py URL DIR [--port 8000] [--ua STRING]

Fetches URL with NetSurf's user agent (what the browser would get), then every
<script src> and <link rel=stylesheet href> of it, into DIR, and rewrites those
references to http://127.0.0.1:PORT/... so that

    python3 -m http.server PORT --bind 127.0.0.1 --directory DIR
    tests/netsurf/run.sh http://127.0.0.1:PORT/index.html

measures the same page every time (count of uncaught script errors, the unimplemented
WebIDL members it uses).  Images and requests made by scripts are not saved.  The copy
is for local measurement only; keep it out of the repository.
"""
import html.parser
import os
import re
import sys
import urllib.parse
import urllib.request

UA = "Mozilla/5.0 (X11; Linux x86_64) NetSurf/3.11"


class Refs(html.parser.HTMLParser):
    def __init__(self):
        super().__init__()
        self.refs = []

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "script" and a.get("src"):
            self.refs.append((a["src"], ".js"))
        elif tag == "link" and a.get("href") and "stylesheet" in (a.get("rel") or "").lower():
            self.refs.append((a["href"], ".css"))


def fetch(url, ua):
    req = urllib.request.Request(url, headers={"User-Agent": ua, "Accept-Language": "en"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def local_name(url, ext):
    """the file name of a saved URL; it ends in ext (.js, .css), so that
    python3 -m http.server sends a type the browser accepts as a script or a
    style sheet (YouTube's app script has no extension)"""
    p = urllib.parse.urlsplit(url)
    path = p.netloc + p.path
    if p.query:
        path += "_" + re.sub(r"[^A-Za-z0-9._-]", "_", p.query)[:80]
    # "=" stays: YouTube checks its script URL for "/k=" (its module paths)
    name = re.sub(r"[^A-Za-z0-9._/=-]", "_", path).strip("/")
    return name if name.endswith(ext) else name + ext


def main():
    args = sys.argv[1:]
    port, ua = "8000", UA
    if "--port" in args:
        i = args.index("--port"); port = args[i + 1]; del args[i:i + 2]
    if "--ua" in args:
        i = args.index("--ua"); ua = args[i + 1]; del args[i:i + 2]
    if len(args) != 2:
        sys.exit(__doc__)
    url, out = args
    os.makedirs(out, exist_ok=True)
    page = fetch(url, ua).decode("utf-8", "replace")
    parser = Refs()
    parser.feed(page)
    done = {}
    for ref, ext in parser.refs:
        absolute = urllib.parse.urljoin(url, html.unescape(ref))
        if absolute in done or not absolute.startswith("http"):
            continue
        name = local_name(absolute, ext)
        dest = os.path.join(out, name)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        try:
            data = fetch(absolute, ua)
        except Exception as e:  # keep going: a missing file is part of the measurement
            print("failed %s: %s" % (absolute, e), file=sys.stderr)
            continue
        with open(dest, "wb") as f:
            f.write(data)
        done[absolute] = "http://127.0.0.1:%s/%s" % (port, name)
        print("%9d %s" % (len(data), name))
    for ref, _ in set(parser.refs):
        absolute = urllib.parse.urljoin(url, html.unescape(ref))
        if absolute in done:
            for spelling in (ref, html.escape(ref)):
                page = page.replace('"%s"' % spelling, '"%s"' % done[absolute])
    with open(os.path.join(out, "index.html"), "w", encoding="utf-8") as f:
        f.write(page)
    print("%9d index.html (%d files)" % (len(page), len(done)))


if __name__ == "__main__":
    main()
