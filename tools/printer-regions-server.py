#!/usr/bin/env python3
# Copyright (C) 2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later
"""Serves the repo for tools/printer-regions-tagger.html and lets its window
review write assets/images/printers/regions.json. Localhost only.

    tools/printer-regions-server.py [port]
    http://localhost:8123/tools/printer-regions-tagger.html?windows
"""
import functools
import http.server
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REGIONS = ROOT / "assets/images/printers/regions.json"


class Handler(http.server.SimpleHTTPRequestHandler):
    def do_POST(self):
        if self.path != "/__regions":
            self.send_error(404)
            return
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        try:
            doc = json.loads(body)
        except ValueError:
            self.send_error(400, "not JSON")
            return
        if not isinstance(doc, dict) or not doc:
            self.send_error(400, "expected a non-empty object")
            return
        tmp = REGIONS.with_suffix(".json.tmp")
        tmp.write_bytes(body)
        os.replace(tmp, REGIONS)
        self.send_response(204)
        self.end_headers()

    def log_message(self, *args):
        pass


port = int(sys.argv[1]) if len(sys.argv) > 1 else 8123
handler = functools.partial(Handler, directory=str(ROOT))
print(f"http://localhost:{port}/tools/printer-regions-tagger.html?windows", flush=True)
http.server.ThreadingHTTPServer(("127.0.0.1", port), handler).serve_forever()
