#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""tests/netsurf/server.py: the HTTP server of the NetSurf test pages.

    tests/netsurf/server.py DIR PORT [PORT...]

Serves the files of DIR and these endpoints on 127.0.0.1 at each PORT (two ports are two
origins, for the cross-origin cases; tests/netsurf/run.sh starts it):

  /api/echo          the request as JSON: method, path, query, headers (lower case), body
  /api/status?code=N&text=T   that status, body "status N"
  /api/redirect?code=N&to=URL  a redirect
  /api/json          a JSON object
  /api/bytes         the bytes 0..255 (application/octet-stream)
  /api/latin1        "café" in ISO-8859-1
  /api/slow?ms=N     answers after N milliseconds
  /api/setcookie?name=N&value=V   sets a cookie (Path=/)

Every endpoint takes acao=VALUE (adds Access-Control-Allow-Origin: VALUE); acao=origin answers
with the request's Origin header and nothing without one, as www.gstatic.com does.
"""
import http.server
import json
import os
import socketserver
import sys
import threading
import time
import urllib.parse


class Handler(http.server.SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def reply(self, status, body, ctype="text/plain; charset=utf-8", extra=(), reason=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        q = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
        self.send_response(status, reason)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        if "acao" in q:
            acao = q["acao"][0]
            if acao == "origin":
                acao = self.headers.get("Origin")
            if acao is not None:
                self.send_header("Access-Control-Allow-Origin", acao)
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def api(self):
        u = urllib.parse.urlsplit(self.path)
        q = {k: v[0] for k, v in urllib.parse.parse_qs(u.query).items()}
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        if u.path == "/api/echo":
            self.reply(200, json.dumps({
                "method": self.command, "path": u.path, "query": q,
                "headers": {k.lower(): v for k, v in self.headers.items()},
                "body": body.decode("utf-8", "replace")}), "application/json",
                [("X-Echo", "yes"), ("X-Multi", "a"), ("X-Multi", "b")])
        elif u.path == "/api/status":
            code = int(q.get("code", "200"))
            self.reply(code, "status %d" % code, reason=q.get("text"))
        elif u.path == "/api/redirect":
            self.reply(int(q.get("code", "302")), "", extra=[("Location", q.get("to", "/api/echo"))])
        elif u.path == "/api/json":
            self.reply(200, json.dumps({"a": 1, "list": [1, 2, 3], "s": "é"}), "application/json")
        elif u.path == "/api/bytes":
            self.reply(200, bytes(range(256)), "application/octet-stream")
        elif u.path == "/api/latin1":
            self.reply(200, "café".encode("iso-8859-1"), "text/plain; charset=iso-8859-1")
        elif u.path == "/api/slow":
            time.sleep(int(q.get("ms", "1000")) / 1000)
            self.reply(200, "slow")
        elif u.path == "/api/setcookie":
            self.reply(200, "set", extra=[("Set-Cookie", "%s=%s; Path=/" % (q.get("name", "c"), q.get("value", "1")))])
        else:
            self.reply(404, "no such endpoint")

    def do_GET(self):
        if self.path.startswith("/api/"):
            self.api()
        else:
            super().do_GET()

    def do_HEAD(self):
        if self.path.startswith("/api/"):
            self.api()
        else:
            super().do_HEAD()

    do_POST = do_PUT = do_DELETE = do_PATCH = do_OPTIONS = api


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    root = os.path.abspath(sys.argv[1])
    os.chdir(root)
    servers = [Server(("127.0.0.1", int(p)), Handler) for p in sys.argv[2:]]
    for s in servers[1:]:
        threading.Thread(target=s.serve_forever, daemon=True).start()
    print("ready", flush=True)
    servers[0].serve_forever()


if __name__ == "__main__":
    main()
