"""Exercise the Ranked GraphQL report and signed SLP upload on macOS, entirely on loopback."""
import gzip
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import shlex
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RankedReportTest(unittest.TestCase):
    def test_two_games_then_completion(self):
        reports = []
        uploads = []
        upload_sizes = []
        statuses = []
        ranks = []

        class Handler(BaseHTTPRequestHandler):
            def send(self, payload):
                body = json.dumps(payload).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_POST(self):
                message = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                if "getRankedMatchPersonalResult" in message["query"]:
                    ranks.append(message["variables"]["request"])
                    self.send({"data": {"getRankedMatchPersonalResult": {"participant": {
                        "ordinal": 1995, "dailyGlobalPlacement": None, "dailyRegionalPlacement": None,
                        "ratingUpdateCount": 7, "ratingChange": 10}}}})
                    return
                report = message["variables"]["report"]
                if "reportOnlineMatchStatus" in message["query"]:
                    statuses.append(report)
                    self.send({"data": {"reportOnlineMatchStatus": True}})
                else:
                    reports.append(report)
                    url = f"http://127.0.0.1:{self.server.server_port}/game/{report['gameIndex']}?signature=a%2Bb&field=x"
                    self.send({"data": {"reportOnlineGame": {"success": True, "uploadUrl": url}}})

            def do_PUT(self):
                compressed = self.rfile.read(int(self.headers["Content-Length"]))
                upload_sizes.append(len(compressed))
                uploads.append((self.path, self.headers["Content-Encoding"], self.headers["Content-Type"],
                                self.headers["X-Goog-Content-Length-Range"], gzip.decompress(compressed)))
                self.send({})

        with tempfile.TemporaryDirectory() as temp, ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
            exe = Path(temp, "report_test")
            iso = Path(temp, "test.iso")
            iso.write_bytes(b"synthetic report test disc")
            flags = ["-Iport/" + inc for inc in ("linux/compat", "linux", "app", "runtime/ppc", "runtime/hle",
                                                "runtime/host", "runtime/gx", "runtime/mu_net", "runtime/abi", "third_party", "third_party/enet/include")]
            sdl_flags = shlex.split(subprocess.check_output(["sdl2-config", "--cflags"], text=True))
            subprocess.run(["/usr/bin/clang++", *sdl_flags, "-std=c++17", "-arch", "x86_64", "-ffunction-sections", "-fdata-sections",
                            "-Wl,-dead_strip", "-Wno-deprecated-declarations", "-DMELEE_LINUX", "-DGX_PC_SETTINGS",
                            "-include", "port/linux/compat/windows.h", *flags,
                            "port/tests/slippi_ranked_report_test.cpp", "port/runtime/host/http_client.cpp", "port/linux/compat/compat.cpp",
                            "port/linux/compat/winhttp_ws.cpp", "-lcurl", "-liconv", "-framework", "Security", "-o", exe], cwd=ROOT, check=True)
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                subprocess.run([exe, f"http://127.0.0.1:{server.server_port}/graphql", iso, temp],
                               env={"HOME": temp, "PATH": os.defpath, "NO_PROXY": "*"}, check=True, timeout=25)
            finally:
                server.shutdown()
                thread.join()
            self.assertEqual([r["gameIndex"] for r in reports], [1, 2])
            for game, report in enumerate(reports, 1):
                self.assertEqual(report["mode"], 0)
                self.assertEqual(report["fbUid"], "test-account")
                self.assertEqual(report["playKey"], "test-key")
                self.assertEqual(report["isoHash"], hashlib.md5(iso.read_bytes()).hexdigest())
                self.assertEqual(len(report["players"]), 4)
                raw = bytes([0x35, 7, 0x38, 4, 0, 0x39, 0, 2, 0x38]) + bytes(1024) + bytes([0x39, 2, game])
                self.assertLess(upload_sizes[game - 1], 128)
                expected = b"{U\x03raw[$U#l" + len(raw).to_bytes(4, "big") + raw + b"U\x08metadata{}}"
                self.assertEqual(uploads[game - 1], (f"/game/{game}?signature=a%2Bb&field=x", "gzip",
                                                     "application/octet-stream", "0,10000000", expected))
            self.assertEqual(ranks, [{"fbUid": "test-account", "playKey": "test-key", "matchId": "mode.ranked-loopback"}])
            self.assertEqual(statuses, [{"fbUid": "test-account", "playKey": "test-key",
                                         "matchId": "mode.ranked-loopback", "status": "normal_completion"}])


if __name__ == "__main__":
    unittest.main()
