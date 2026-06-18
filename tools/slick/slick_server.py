#!/usr/bin/env python3
"""slick_server.py — static server for Slick's dist/ that sends no-cache headers
so Electron/browsers always pull the freshest build after a deploy (the bare
`python -m http.server` cached aggressively, leaving stale UI on reload)."""

import os
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

DIST = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dist")


class NoCacheHandler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()

    def log_message(self, *args):
        pass  # quiet


if __name__ == "__main__":
    os.chdir(DIST)
    ThreadingHTTPServer(("127.0.0.1", 8090), NoCacheHandler).serve_forever()
