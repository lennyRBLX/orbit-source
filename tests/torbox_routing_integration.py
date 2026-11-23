"""Download routing through a loopback TorBox and provider fixture."""

import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.error import URLError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]
BINARY = os.environ.get("ORBIT_TEST_BINARY", ROOT / "build/orbit-test")
FILE = b"\x7fFIH" + b"test" * 1023


class Fixture(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, payload, status=200, headers=None):
        self.send_response(status)
        self.send_header("Content-Length", str(len(payload)))
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(payload)

    def api(self, data, status=200):
        self.reply(json.dumps({"success": status == 200, "data": data}).encode(), status,
                   {"Content-Type": "application/json"})

    def do_POST(self):
        self.rfile.read(int(self.headers["Content-Length"]))
        if self.path != "/webdl/createwebdownload":
            self.reply(b"", 404)
            return
        self.server.calls.append("create")
        self.api(None, 400)

    def do_GET(self):
        path = urlsplit(self.path).path
        if path == "/user/me":
            self.api({"id": 1})
        elif path == "/webdl/hosters":
            self.api(self.server.hosters)
        elif path == "/webdl/mylist":
            self.server.calls.append("list")
            self.api(self.server.downloads)
        elif path == "/webdl/requestdl":
            self.api(self.server.base + "/torbox-file")
        elif path == "/catalogue.json":
            packages = [{"title": "Provider page fixture", "titleId": "PPSA98765", "category": "game",
                         "downloadLinks": [{"name": "Provider", "url": self.server.base + "/provider"}]}]
            self.reply(json.dumps({"packages": packages}).encode(), headers={"Content-Type": "application/json"})
        elif path == "/provider":
            self.server.calls.append(path)
            self.reply(b"", 302, {"Location": "/original"})
        elif path in ("/original", "/torbox-file"):
            self.server.calls.append(path)
            if path == "/original" and self.server.original_fails:
                self.reply(b"", 404)
                return
            start, end = 0, len(FILE) - 1
            requested = self.headers.get("Range")
            headers = {"Content-Type": "application/octet-stream", "ETag": json.dumps(path)}
            if self.server.browser and path == "/original":
                headers["Content-Disposition"] = 'attachment; filename="provider.pkg"'
            if requested:
                first, last = requested.removeprefix("bytes=").split("-")
                start, end = int(first), int(last) if last else end
                headers["Content-Range"] = f"bytes {start}-{end}/{len(FILE)}"
            payload = FILE[start:end + 1]
            if path == "/torbox-file" and self.server.truncate and len(payload) > 1:
                self.send_response(206 if requested else 200)
                self.send_header("Content-Length", str(len(payload)))
                for key, value in headers.items():
                    self.send_header(key, value)
                self.end_headers()
                self.wfile.write(payload[:len(payload) // 2])
                return
            self.reply(payload, 206 if requested else 200, headers)
        else:
            self.reply(b"", 404)

    do_HEAD = do_GET


def wait_for(check, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        time.sleep(0.1)
    raise AssertionError("Timed out waiting for Orbit")


class TorBoxRouting(unittest.TestCase):
    def run_download(self, hosters, *, cached=False, truncate=False, original_fails=False, explicit=None,
                     browser=False):
        with tempfile.TemporaryDirectory() as temp, ThreadingHTTPServer(("127.0.0.1", 0), Fixture) as fixture:
            fixture.base = f"http://127.0.0.1:{fixture.server_port}"
            fixture.calls = []
            fixture.hosters = hosters
            fixture.downloads = []
            fixture.original_fails = original_fails
            fixture.truncate = truncate
            fixture.browser = browser
            thread = threading.Thread(target=fixture.serve_forever, daemon=True)
            thread.start()
            try:
                with socket.socket() as sock:
                    sock.bind(("127.0.0.1", 0))
                    port = sock.getsockname()[1]
                root = Path(temp).resolve()
                drive = root / "drive"
                drive.mkdir()
                (drive / "homebrew").mkdir()
                original_url = fixture.base + "/original"
                env = dict(os.environ, ORBIT_TEST_URL=original_url, ORBIT_TEST_SIZE=str(len(FILE)),
                           ORBIT_TEST_SOURCE="archive", ORBIT_TEST_TORBOX_URL=fixture.base)
                with (root / "log").open("w+") as log:
                    service = subprocess.Popen(
                        [BINARY, "--port", str(port), "--storage", str(drive), "--state", str(root / "state")],
                        cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                    )
                    try:
                        self.check_download(port, fixture, original_url, cached, explicit, drive)
                    except Exception:
                        log.seek(0)
                        print(log.read())
                        raise
                    finally:
                        service.terminate()
                        service.wait(timeout=15)
            finally:
                fixture.shutdown()
                thread.join()
        return fixture.calls

    def check_download(self, port, fixture, original_url, cached, explicit, drive):
        token = ""

        def call(path, body=None):
            headers = {"Content-Type": "application/json"}
            if token:
                headers["Authorization"] = "Bearer " + token
            data = None if body is None else json.dumps(body).encode()
            with urlopen(Request(f"http://127.0.0.1:{port}/api/v1{path}", data, headers), timeout=3) as reply:
                return json.load(reply)

        def connect():
            try:
                return call("/session")["token"]
            except URLError:
                return None

        token = wait_for(connect)
        sources = call("/sources")
        call("/sources", {"enabled": ["remote"] if fixture.browser else ["archive"], "acknowledged": True,
                          "noticeVersion": sources["noticeVersion"]})
        if fixture.browser:
            call("/remote-sources", {"url": fixture.base + "/catalogue.json"})
        title = "Provider page fixture" if fixture.browser else "Orbit local test file"
        release = wait_for(lambda: next((r for r in call("/catalog") if r["title"] == title), None))
        call("/debrid", {"action": "connect", "apiKey": "orbit-test-fixture-key"})
        wait_for(lambda: call("/debrid")["connected"])
        if cached:
            fixture.downloads = [{
                "id": 2, "original_url": original_url, "download_present": True, "download_finished": True,
                "files": [{"id": 0, "short_name": release["filename"], "size": len(FILE)}],
            }]
        download = {"releaseId": release["id"], "storageId": "desktop"}
        if explicit:
            download["delivery"] = explicit
        job_id = call("/downloads", download)["id"]

        def finished():
            job, = call("/downloads")
            self.assertEqual(job["id"], job_id)
            return job if job["status"] in ("complete", "error", "cancelled") else None

        job = wait_for(finished)
        uses_torbox = cached and not fixture.truncate and "/torbox-file" in fixture.calls
        self.assertEqual(job["delivery"], "torbox" if uses_torbox else "direct")
        self.assertEqual(job["status"], "error" if fixture.original_fails else "complete", job)
        if job["status"] == "complete":
            self.assertEqual((drive / "homebrew" / job["filename"]).read_bytes(), FILE)

    def test_default_tries_supported_hosts_first(self):
        hosters = [{"domains": ["127.0.0.1"], "status": True}]
        calls = self.run_download(hosters, cached=True)
        self.assertEqual(calls[0], "list")
        self.assertIn("/torbox-file", calls)
        self.assertNotIn("/original", calls)

    def test_unsupported_and_offline_hosts_skip_torbox(self):
        for domains, status in ((["example.org"], True), (["127.0.0.1"], False)):
            with self.subTest(domains=domains, status=status):
                calls = self.run_download([{"domains": domains, "status": status}], explicit="torbox")
                self.assertTrue(calls)
                self.assertEqual(set(calls), {"/original"})

    def test_rejected_torbox_link_uses_original_even_if_original_fails(self):
        hosters = [{"domains": ["127.0.0.1"], "status": True}]
        for original_fails in (False, True):
            with self.subTest(original_fails=original_fails):
                calls = self.run_download(hosters, original_fails=original_fails)
                self.assertEqual(calls[:3], ["list", "create", "/original"])

    def test_torbox_partial_restarts_from_original(self):
        calls = self.run_download([{"domains": ["127.0.0.1"], "status": True}],
                                  cached=True, truncate=True)
        self.assertEqual(calls[:4], ["list", "/torbox-file", "/torbox-file", "/original"])

    def test_torbox_rejection_resolves_the_original_provider_page(self):
        calls = self.run_download([{"domains": ["127.0.0.1"], "status": True}], browser=True)
        self.assertEqual(calls[:3], ["list", "create", "/provider"])
        self.assertIn("/original", calls)


if __name__ == "__main__":
    unittest.main()
