#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A stand-in for SSBM Nucleus on this computer, for test runs of opponents' skins online
(host/skin_fetch.h), on 127.0.0.1 only. Prints each request.

    python -I -B tools/skin_fetch_standin.py [--rate <bytes/s>] [--fail <status>] [--export] [--view-400]
        <port> <mod>:<file>:<path> [...]

Serves, for each given file:
  GET /api/public/v1/mods/<mod>                               the post, as the API answers it, with that file
  GET /api/public/v1/mods/<mod>/download?file=<file>[&view=1] a redirect to /media/<mod>-<file>, as the site does
  GET /api/posts/<mod>/export-nucleus/<file>[?view=1]         the site's per-file export: a zip of the file
  GET /media/<mod>-<file>                                     the bytes
With --export the post gives the file no download_url (as a file uploaded through the site), so the
game takes the export. Both routes count a download of the post, except with view=1 (an opponent's
skin shown, not installed: the game sends it; an install from the Mods menu never does); any other
parameter there is refused (400). Each prints which it was.
--view-400: the download route answers 400 to view=1, as the live site did before it took the
marker; the game then asks once more without it (and logs one line), which counts.

Point a --hidden game run at it with MELEE_TEST_MOD_BROWSER_API=http://127.0.0.1:<port>/api/public/v1.
Prints the SHA-256 of each file on start, for the test hook that names it.

For the receiver's bounds: --rate sends each file at most that many bytes a second (a slow link),
--fail answers every file request with that HTTP status instead (a site that refuses or is down).
"""
import hashlib
import io
import json
import sys
import time
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ARGS = sys.argv[1:]
RATE = FAIL = 0
EXPORT = VIEW_400 = False
while ARGS and ARGS[0] in ("--rate", "--fail", "--export", "--view-400"):
    if ARGS[0] in ("--export", "--view-400"):
        EXPORT = EXPORT or ARGS[0] == "--export"
        VIEW_400 = VIEW_400 or ARGS[0] == "--view-400"
        ARGS = ARGS[1:]
        continue
    if ARGS[0] == "--rate":
        RATE = int(ARGS[1])
    else:
        FAIL = int(ARGS[1])
    ARGS = ARGS[2:]
PORT = int(ARGS[0])
FILES = {}
for spec in ARGS[1:]:
    mod, file_id, path = spec.split(":", 2)
    data = open(path, "rb").read()
    FILES[(int(mod), int(file_id))] = data
    print("mod %s file %s: %d bytes sha256 %s (%s)" % (mod, file_id, len(data), hashlib.sha256(data).hexdigest(), path), flush=True)
BASE = "http://127.0.0.1:%d" % PORT
DOWNLOADS = {}   # mod -> downloads counted


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        headers = getattr(self, "headers", None)
        agent = headers.get("User-Agent", "-") if headers else "-"
        sys.stdout.write("%s (User-Agent: %s)\n" % (fmt % args, agent))
        sys.stdout.flush()

    def answer(self, status, body=b"", kind="application/octet-stream", location=None):
        self.send_response(status)
        if location:
            self.send_header("Location", location)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def counted(self, route, key, query, allowed):
        """Whether this was a view (view=1), else a download of the post is counted. None: a parameter
        not in `allowed`, or another view, refused with 400 (sent)."""
        if set(query) - allowed or query.get("view", ["1"]) != ["1"]:
            self.answer(400, b'{"error": "unknown parameter"}', "application/json")
            return None
        view = "view" in query
        if not view:
            DOWNLOADS[key[0]] = DOWNLOADS.get(key[0], 0) + 1
        print("%s: mod %d file %d, %s; downloads of the post now %d" % (
            route, key[0], key[1], "a view (view=1), not counted" if view else "counted as a download",
            DOWNLOADS.get(key[0], 0)), flush=True)
        return view

    def file_bytes(self, data):
        if FAIL:
            return self.answer(FAIL)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if not RATE:
            self.wfile.write(data)
            return
        chunk = max(1, RATE // 10)
        try:
            for at in range(0, len(data), chunk):
                self.wfile.write(data[at:at + chunk])
                self.wfile.flush()
                time.sleep(0.1)
        except OSError:   # the game gave up on it
            pass

    def do_GET(self):
        url = urlparse(self.path)
        parts = url.path.strip("/").split("/")
        query = parse_qs(url.query, keep_blank_values=True)
        nums = lambda *xs: all(x.isdigit() for x in xs)
        if len(parts) == 5 and parts[:4] == ["api", "public", "v1", "mods"] and nums(parts[4]):
            mod = int(parts[4])
            files = [{"id": f, "filename": "%d.dat" % f, "file_type": "character_dat",
                      "download_url": None if EXPORT else "%s/api/public/v1/mods/%d/download?file=%d" % (BASE, mod, f),
                      "export_url": "%s/api/posts/%d/export-nucleus/%d" % (BASE, mod, f)}
                     for (m, f) in FILES if m == mod]
            if files:
                return self.answer(200, json.dumps({"data": {"id": mod, "title": "stand-in", "files": files}}).encode(),
                                   "application/json")
        if len(parts) == 6 and parts[:4] == ["api", "public", "v1", "mods"] and parts[5] == "download":
            file_id = query.get("file", ["0"])[0]
            if nums(parts[4], file_id) and (int(parts[4]), int(file_id)) in FILES:
                key = (int(parts[4]), int(file_id))
                if VIEW_400 and "view" in query:
                    print("download: mod %d file %d, view refused (400, --view-400)" % key, flush=True)
                    return self.answer(400, b'{"error": "unknown parameter: view"}', "application/json")
                if self.counted("download", key, query, {"view", "file"}) is None:
                    return None
                return self.answer(302, location="%s/media/%s-%s" % (BASE, parts[4], file_id))
        if len(parts) == 5 and parts[:2] == ["api", "posts"] and parts[3] == "export-nucleus" and nums(parts[2], parts[4]):
            key = (int(parts[2]), int(parts[4]))
            if key in FILES:
                if self.counted("export", key, query, {"view"}) is None:
                    return None
                buf = io.BytesIO()
                with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as z:
                    z.writestr("%d.dat" % key[1], FILES[key])
                return self.file_bytes(buf.getvalue())
        if len(parts) == 2 and parts[0] == "media":
            mod, _, file_id = parts[1].partition("-")
            data = FILES.get((int(mod), int(file_id))) if nums(mod, file_id) else None
            if data is not None:
                return self.file_bytes(data)
        self.answer(404)


ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
