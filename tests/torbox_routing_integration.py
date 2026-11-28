"""Download routing through a loopback TorBox and provider fixture."""

import hashlib
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
from urllib.error import HTTPError, URLError
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
        if self.server.rejection == "missing":
            self.reply(json.dumps({"success": False, "error": "SOURCE_NOT_FOUND",
                                   "detail": "File not found"}).encode(), 400,
                       {"Content-Type": "application/json"})
        else:
            self.api(None, 400)

    def do_GET(self):
        path = urlsplit(self.path).path
        if path == "/user/me":
            self.api({"id": 1} if self.server.key != "invalid" else None,
                     401 if self.server.key == "invalid" else 200)
        elif path == "/webdl/hosters":
            self.api(self.server.hosters)
        elif path == "/webdl/mylist":
            self.server.calls.append("list")
            self.server.polls += 1
            if self.server.transient and self.server.polls == 1:
                self.api(None, 503)
                return
            items = self.server.downloads
            if self.server.processing:
                first_item_poll = 2 if self.server.transient else 1
                if self.server.polls == first_item_poll or self.server.processing == "hold":
                    items = [dict(item, download_present=False, download_finished=False,
                                  download_state="downloading", progress=0.2) for item in items]
                elif self.server.processing == "failed":
                    items = [dict(item, download_present=False, download_finished=False,
                                  download_state="failed", error="upstream failed") for item in items]
            self.api(items)
        elif path == "/webdl/requestdl":
            self.api(self.server.base + "/torbox-file")
        elif path == "/ps5/ps5-games-list":
            self.reply(b'<a href="/ps5/cyberpunk-2077-ultimate-edition-13811.html">Cyberpunk 2077 Ultimate Edition</a>')
        elif path == "/ps5/download-cyberpunk-2077-ultimate-edition-13811.html":
            if getattr(self.server, "pspkg_missing", False):
                self.reply(b"<title>Missing</title>", 404, {"Content-Type": "text/html"})
            else:
                headings = ["PPSA04029 v02.120 EUR PKG", "PPSA04029 v02.120 EUR PKG",
                            "PPSA04029 v02.120 EUR PKG", "PPSA04027 ZSTD", "PPSA04029 PKG"]
                gates = "".join(f'<h3 class="dpx-edition-title">{heading}</h3>'
                                f'<div class="dpx-gate" data-field-id="{i}">Awaiting verification</div>'
                                for i, heading in enumerate(headings, 1))
                self.reply(gates.encode(), headers={"Content-Type": "text/html"})
        elif path == "/ps5/download.php":
            self.server.calls.append(self.path)
            routes = {
                "1": "/pspkg-provider/vikingfile.com/file.pkg",
                "5": "/pspkg-provider/vikingfile.com/rejected.pkg",
                "2": "http://127.0.0.2/file.pkg",
                "3": "/pspkg-provider/buzzheavier.com/file.pkg",
            }
            if self.path.endswith("route=expired"):
                self.reply(b"<p>Verification required</p>", 403, {"Content-Type": "text/html"})
            else:
                self.reply(b"", 302, {"Location": routes[self.path.rsplit("=", 1)[-1]]})
        elif path == "/catalogue.json":
            packages = [{"title": "Provider page fixture", "titleId": "PPSA98765", "category": "game",
                         "downloadLinks": [{"name": "Provider", "url": self.server.base + "/provider"}]}]
            if self.server.mirrors:
                packages[0].update(filename="provider.pkg", sizeBytes=len(FILE), checksum=hashlib.sha256(FILE).hexdigest(),
                                   downloadLinks=[{"name": f"Mirror {i}", "url": f"{self.server.base}/mirror-{i}.pkg"}
                                                  for i in (1, 2)])
            self.reply(json.dumps({"packages": packages}).encode(), headers={"Content-Type": "application/json"})
        elif path == "/provider":
            self.server.calls.append(path)
            if self.server.original_fails:
                self.reply(b"<h1>FiLe <b>not</b> found</h1>", 404, {"Content-Type": "text/html"})
            else:
                self.reply(b"", 302, {"Location": "/original"})
        elif path in ("/original", "/torbox-file", "/mirror-1.pkg", "/mirror-2.pkg") or path.startswith("/pspkg-provider/"):
            self.server.calls.append(path)
            if (path == "/original" and self.server.original_fails) or path == self.server.dead_mirror:
                self.reply(b"<h1>File not found</h1>", 404, {"Content-Type": "text/html"})
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
                     browser=False, processing=None, rejection=None, key="valid", interrupt=None,
                     transient=False, mirrors=False, pspkg=None):
        with tempfile.TemporaryDirectory() as temp, ThreadingHTTPServer(("127.0.0.1", 0), Fixture) as fixture:
            fixture.base = f"http://127.0.0.1:{fixture.server_port}"
            fixture.calls = []
            fixture.hosters = hosters
            fixture.downloads = []
            fixture.original_fails = original_fails
            fixture.truncate = truncate
            fixture.browser = browser or mirrors
            fixture.mirrors = mirrors
            fixture.dead_mirror = None
            fixture.processing = processing
            fixture.rejection = rejection
            fixture.key = key
            fixture.polls = 0
            fixture.transient = transient
            fixture.pspkg_missing = pspkg == "missing"
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
                if fixture.browser:
                    env["ORBIT_TEST_CATALOGUE_URL"] = fixture.base + "/catalogue.json"
                if pspkg:
                    anchor = lambda value: f'<a href="download.php?route={value}">Download</a>'
                    reveal = {"success": True, "fields": {
                        str(i): anchor(link) for i, link in enumerate(("1", "2", "3", "5", "expired"), 1)
                    }}
                    reveal["fields"]["999"] = '<a href="download.php?route=not-this-listing">Unrelated</a>'
                    env.update(ORBIT_TEST_PSPKG_PS5_URL=fixture.base + "/ps5/",
                               ORBIT_TEST_PSPKG_HTML=json.dumps(reveal))
                    if pspkg == "cancel":
                        env["ORBIT_TEST_PSPKG_DELAY_MS"] = "1500"
                    if pspkg == "expire":
                        env["ORBIT_TEST_PSPKG_TTL"] = "0"
                    if pspkg == "empty":
                        env["ORBIT_TEST_PSPKG_HTML"] = json.dumps({"success": True, "fields": {"1": "<p>No links</p>"}})
                with (root / "log").open("w+") as log:
                    service = subprocess.Popen(
                        [BINARY, "--port", str(port), "--storage", str(drive), "--state", str(root / "state")],
                        cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                    )
                    try:
                        if pspkg:
                            self.check_pspkg(port, fixture, pspkg, drive)
                        else:
                            self.check_download(port, fixture, original_url, cached, explicit, drive, interrupt)
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

    def check_pspkg(self, port, fixture, mode, drive):
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
        call("/debrid", {"action": "connect", "apiKey": "orbit-test-fixture-key"})
        wait_for(lambda: call("/debrid")["connected"])
        release = wait_for(lambda: next((r for r in call("/catalog") if r["provider"] == "PSPKG"), None))
        title = call("/community-title?release=" + release["id"])
        self.assertEqual([row["id"] for row in title["pspkg"]], [release["id"]])
        status = call("/browser", {"action": "pspkg-links", "releaseId": release["id"],
                                    "gameId": release["gameId"]})
        if mode == "cancel":
            call("/browser", {"action": "cancel", "id": status["id"]})
        status = wait_for(lambda: result if not (result := call("/browser"))["active"] else None)
        self.assertEqual(status["pspkgGameId"], release["gameId"])
        if mode in ("cancel", "expire", "empty", "missing"):
            expected = {"cancel": "cancelled", "expire": "expired", "empty": "no-anchors", "missing": "missing-page"}
            self.assertEqual(status["pspkgStatus"], expected[mode])
            self.assertEqual(call("/downloads"), [])
            if mode == "expire":
                with self.assertRaises(HTTPError) as rejected:
                    call("/browser", {"action": "pspkg-download", "releaseId": release["id"],
                                      "linkId": "1", "storageId": "desktop"})
                self.assertEqual(rejected.exception.code, 409)
                rejected.exception.close()
            return
        self.assertEqual(status["pspkgStatus"], "partial")
        choices = status["pspkgLinks"]
        self.assertIn("unsafe-url", [link["reason"] for link in choices])
        self.assertIn("verify-required", [link["reason"] for link in choices])
        self.assertIn("unsupported-format", [link["reason"] for link in choices])
        self.assertNotIn("download.php", json.dumps(choices))
        available = [link for link in choices if not link["reason"]]
        self.assertEqual(len(available), 2)
        link = available[0 if mode == "viking" else 1]
        self.assertEqual((link["titleId"], link["version"], link["region"], link["format"]),
                         ("PPSA04029", "02.120", "EUR", "PKG"))
        self.assertEqual(link.get("torboxAvailable"), mode == "supported")
        if mode == "supported":
            fixture.downloads = [{
                "id": 2, "original_url": fixture.base + "/pspkg-provider/buzzheavier.com/file.pkg",
                "download_present": True, "download_finished": True,
                "files": [{"id": 0, "short_name": "file.pkg", "size": len(FILE)}],
            }]
        call("/browser", {"action": "pspkg-download", "releaseId": release["id"],
                          "linkId": link["id"], "storageId": "desktop", "delivery": "torbox"})
        browser = wait_for(lambda: value if not (value := call("/browser"))["active"] else None)
        self.assertEqual(browser["state"], "queued", browser)
        job = wait_for(lambda: next((j for j in call("/downloads") if
                                     j["status"] in ("complete", "error")), None))
        self.assertEqual(job["status"], "complete", job)
        self.assertEqual(job["delivery"], "torbox" if mode == "supported" else "direct")
        self.assertEqual((drive / "homebrew" / job["filename"]).read_bytes(), FILE)

    def test_pspkg_uses_resolved_provider_coverage_for_both_routes(self):
        hosts = [{"domains": ["buzzheavier.com"], "status": True}]
        direct = self.run_download(hosts, pspkg="viking")
        self.assertNotIn("create", direct)
        self.assertNotIn("list", direct)
        covered = self.run_download(hosts, pspkg="supported")
        self.assertIn("list", covered)
        self.assertIn("/torbox-file", covered)

    def test_pspkg_handoff_cancellation_expiry_and_landing_states(self):
        for mode in ("cancel", "expire", "empty", "missing"):
            with self.subTest(mode=mode):
                self.run_download([], pspkg=mode)

    def check_download(self, port, fixture, original_url, cached, explicit, drive, interrupt):
        token = ""

        def call(path, body=None, method=None):
            headers = {"Content-Type": "application/json"}
            if token:
                headers["Authorization"] = "Bearer " + token
            data = None if body is None else json.dumps(body).encode()
            with urlopen(Request(f"http://127.0.0.1:{port}/api/v1{path}", data, headers, method=method),
                         timeout=3) as reply:
                return json.load(reply)

        def connect():
            try:
                return call("/session")["token"]
            except URLError:
                return None

        token = wait_for(connect)
        if fixture.browser:
            for path, method, body, code in (
                ("/sources", "POST", {"enabled": [], "noticeVersion": 1, "acknowledged": True}, 405),
                ("/remote-sources", "POST", {"url": fixture.base + "/catalogue.json"}, 404),
                ("/remote-sources/dlps", "PATCH", {"enabled": False}, 405),
                ("/remote-sources/dlps", "DELETE", None, 405),
            ):
                with self.subTest(path=path, method=method):
                    with self.assertRaises(HTTPError) as rejected:
                        call(path, body, method)
                    self.assertEqual(rejected.exception.code, code)
                    rejected.exception.close()
        title = "Provider page fixture" if fixture.browser else "Orbit local test file"
        release = wait_for(lambda: next((r for r in call("/catalog") if r["title"] == title), None))
        if fixture.browser:
            self.assertGreater(call("/system")["remoteGeneration"], 1)
        if fixture.mirrors:
            self.assertEqual(release["mirrorCount"], 2)
            primary = int(release["provider"].removeprefix("Mirror "))
            fixture.dead_mirror = f"/mirror-{primary}.pkg"
            original_url = fixture.base + f"/mirror-{3 - primary}.pkg"
        elif fixture.browser:
            original_url = fixture.base + "/provider"
        if fixture.key != "missing":
            call("/debrid", {"action": "connect", "apiKey": "orbit-test-fixture-key"})
            if fixture.key == "invalid":
                wait_for(lambda: not call("/debrid")["busy"])
                self.assertFalse(call("/debrid")["connected"])
            else:
                wait_for(lambda: call("/debrid")["connected"])
        if cached:
            fixture.downloads = [{
                "id": 2, "original_url": original_url,
                "download_present": True, "download_finished": True,
                "files": [{"id": 0, "short_name": "provider.pkg" if fixture.browser else release["filename"],
                           "size": len(FILE)}],
            }]
        download = {"releaseId": release["id"], "storageId": "desktop"}
        if explicit == "browser":
            call("/browser", dict(download, action="start", returnTarget="browser"))
            job_id = wait_for(lambda: call("/browser")["jobId"])
        else:
            if explicit:
                download["delivery"] = explicit
            job_id = call("/downloads", download)["id"]

        terminal = {"complete", "error", "cancelled"}
        if interrupt:
            wait_for(lambda: fixture.polls == 1 and call("/downloads")[0]["status"] == "queued")
            if interrupt == "disconnect":
                call("/debrid", {"action": "disconnect"})
                terminal.add("paused")
            elif interrupt == "disable":
                call("/sources", {"enabled": [], "acknowledged": True, "noticeVersion": sources["noticeVersion"]})
                terminal.add("paused")
            else:
                call(f"/downloads/{job_id}/cancel", {})

        def finished():
            job, = call("/downloads")
            self.assertEqual(job["id"], job_id)
            return job if job["status"] in terminal else None

        job = wait_for(finished)
        if interrupt:
            self.assertEqual(job["status"], "cancelled" if interrupt == "cancel" else "paused")
            self.assertEqual(fixture.calls, ["list"])
            return
        uses_torbox = cached and not fixture.truncate and "/torbox-file" in fixture.calls
        self.assertEqual(job["delivery"], "torbox" if uses_torbox else "direct")
        self.assertEqual(job["status"], "error" if fixture.original_fails and not uses_torbox else "complete", job)
        if job["status"] == "complete":
            self.assertEqual((drive / "homebrew" / job["filename"]).read_bytes(), FILE)

    def test_default_tries_supported_hosts_first(self):
        hosters = [{"domains": ["127.0.0.1"], "status": True}]
        calls = self.run_download(hosters, cached=True)
        self.assertEqual(calls[0], "list")
        self.assertIn("/torbox-file", calls)
        self.assertNotIn("/original", calls)

    def test_processing_is_polled_before_using_the_cached_file(self):
        calls = self.run_download([{"domains": ["127.0.0.1"], "status": True}],
                                  cached=True, processing="ready")
        self.assertEqual(calls[:3], ["list", "list", "/torbox-file"])
        self.assertNotIn("/original", calls)
        self.assertNotIn("create", calls)

    def test_processing_failure_falls_back_to_the_original(self):
        calls = self.run_download([{"domains": ["127.0.0.1"], "status": True}],
                                  cached=True, processing="failed")
        self.assertEqual(calls[:3], ["list", "list", "/original"])
        self.assertNotIn("/torbox-file", calls)

    def test_torbox_missing_falls_back_and_both_missing_terminates(self):
        hosters = [{"domains": ["127.0.0.1"], "status": True}]
        for provider_missing in (False, True):
            with self.subTest(provider_missing=provider_missing):
                calls = self.run_download(hosters, rejection="missing", original_fails=provider_missing, browser=True)
                self.assertEqual(calls[:3], ["list", "create", "/provider"])
                self.assertEqual(calls.count("list"), 1)
                self.assertEqual(calls.count("create"), 1)
                if provider_missing:
                    self.assertEqual(calls.count("/provider"), 1)
                    self.assertNotIn("/original", calls)
                else:
                    self.assertIn("/original", calls)

    def test_direct_missing_uses_torbox_cache_from_file_and_provider_page(self):
        hosters = [{"domains": ["127.0.0.1"], "status": True}]
        for browser, delivery in ((False, "direct"), (True, "direct"), (True, "browser")):
            with self.subTest(browser=browser, delivery=delivery):
                calls = self.run_download(hosters, explicit=delivery, cached=True, original_fails=True, browser=browser)
                self.assertEqual(calls[0], "/provider" if browser else "/original")
                self.assertEqual(calls.count("list"), 1)
                self.assertIn("/torbox-file", calls)
                self.assertNotIn("create", calls)

    def test_indeterminate_torbox_response_allows_one_cache_recheck(self):
        calls = self.run_download([{"domains": ["127.0.0.1"], "status": True}],
                                  cached=True, transient=True, original_fails=True, browser=True)
        self.assertEqual(calls[:4], ["list", "/provider", "list", "/torbox-file"])
        self.assertNotIn("create", calls)

    def test_indeterminate_torbox_response_can_recover_an_item_still_processing(self):
        calls = self.run_download([{"domains": ["127.0.0.1"], "status": True}],
                                  cached=True, transient=True, processing="ready", original_fails=True, browser=True)
        self.assertEqual(calls[:5], ["list", "/provider", "list", "list", "/torbox-file"])
        self.assertNotIn("create", calls)

    def test_dead_primary_mirror_keeps_torbox_first_for_the_remaining_mirror(self):
        calls = self.run_download([{"domains": ["127.0.0.1"], "status": True}],
                                  mirrors=True, cached=True, rejection="missing")
        attempted_mirror = calls[2]
        self.assertIn(attempted_mirror, ("/mirror-1.pkg", "/mirror-2.pkg"))
        self.assertEqual(calls[:6], ["list", "create", attempted_mirror, attempted_mirror, "list", "/torbox-file"])
        self.assertEqual(calls.count("create"), 1)

    def test_missing_and_rejected_keys_skip_torbox(self):
        for key in ("missing", "invalid"):
            with self.subTest(key=key):
                calls = self.run_download([{"domains": ["127.0.0.1"], "status": True}], key=key)
                self.assertEqual(set(calls), {"/original"})

    def test_waiting_is_interruptible(self):
        hosters = [{"domains": ["127.0.0.1"], "status": True}]
        for interrupt in ("cancel", "disconnect"):
            with self.subTest(interrupt=interrupt):
                self.run_download(hosters, cached=True, processing="hold", interrupt=interrupt)

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
