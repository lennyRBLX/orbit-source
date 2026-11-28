"""Host tests for the Pegasus adapter, worker, durable cache and catalogue view."""

import copy
import http.server
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = json.loads((ROOT / "tests/fixtures/pegasus-catalog.json").read_text())
DRIVER = ROOT / "build/orbit-remote-catalog-test"
DEFAULTS = {"dlps", "evox-coreos", "pfs", "pippo-exfat", "zer0", "padrino",
            "pspkg-ps5", "pspkg-ps4"}


class SourceHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, _format, *args):
        pass

    def do_GET(self):
        with self.server.lock:
            validator = self.headers.get("If-None-Match")
            self.server.requests.append(validator)
            self.server.paths.append(self.path)
            self.server.ranges.append((self.path, self.headers.get("Range")))
            if self.path.startswith("/api/files/"):
                self.server.page_tokens.append((self.path, self.headers.get("X-Page-Token")))
            extra = self.server.downloads.get(self.path)
            if extra:
                status, headers, body = extra
            elif self.server.failure:
                status = self.server.failure
                body = b"{broken"
            elif validator == '"fixture-v1"':
                status, body = 304, b""
            else:
                status, body = 200, json.dumps(self.server.feeds.get(self.path, self.server.feed)).encode()
        self.send_response(status)
        if extra:
            for name, value in headers.items():
                self.send_header(name, value)
        if self.server.send_etag and not extra:
            self.send_header("ETag", '"fixture-v1"')
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)


class FixtureServer(http.server.ThreadingHTTPServer):
    def __init__(self):
        super().__init__(("127.0.0.1", 0), SourceHandler)
        self.lock = threading.Lock()
        self.requests = []
        self.paths = []
        self.ranges = []
        self.page_tokens = []
        self.failure = None
        self.feed = None
        self.feeds = {}
        self.downloads = {}
        self.send_etag = True
        self.url = f"http://127.0.0.1:{self.server_port}/catalogue.json"


class Driver:
    def __init__(self, directory, feed=None, second_feed=None):
        env = dict(os.environ)
        if feed:
            env["ORBIT_TEST_CATALOGUE_URL"] = feed
        if second_feed:
            env["ORBIT_TEST_SECOND_CATALOGUE_URL"] = second_feed
        self.process = subprocess.Popen(
            [DRIVER, directory], cwd=ROOT, env=env, text=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE
        )
        self.first_game = json.loads(self.process.stdout.readline())

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.process.stdin.close()
        try:
            status = self.process.wait(timeout=10)
            if status:
                raise AssertionError(f"Remote catalogue driver exited with status {status}")
        finally:
            self.process.stdout.close()
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()

    def command(self, op, **fields):
        self.process.stdin.write(json.dumps({"op": op, **fields}) + "\n")
        self.process.stdin.flush()
        return json.loads(self.process.stdout.readline())

    def idle(self):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            result = self.command("show")
            if not result["refreshing"]:
                return result
            time.sleep(0.015)
        raise AssertionError("Remote catalogue worker did not go idle")


def source(snapshot, source_id):
    return next(row for row in snapshot["sources"] if row["id"] == source_id)


class RemoteCatalogueTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        args = ["make", "build/orbit-remote-catalog-test"]
        if platform.system() == "Darwin":
            llvm = os.environ.get("LLVM_PREFIX")
            openssl = os.environ.get("OPENSSL_PREFIX")
            if shutil.which("brew"):
                llvm = llvm or subprocess.check_output(["brew", "--prefix", "llvm@19"], text=True).strip()
                openssl = openssl or subprocess.check_output(["brew", "--prefix", "openssl@3"], text=True).strip()
            if llvm:
                args.append(f"HOST_CC={llvm}/bin/clang")
            args.append("CFLAGS=-std=c11 -O2 -Wall -Wextra -Werror -Wno-misleading-indentation -D_DARWIN_C_SOURCE"
                        + (f" -I{openssl}/include" if openssl else ""))
            if openssl:
                args.append(f"LDFLAGS=-L{openssl}/lib")
        subprocess.run(args, cwd=ROOT, check=True)

    def test_padrino_snapshot_adds_ps4_and_ps5_games_without_replacing_orbit(self):
        with tempfile.TemporaryDirectory() as directory, Driver(directory) as service:
            state = source(service.command("show"), "padrino")
            self.assertGreater(state["entryCount"], 10000)
            view = service.command("view", sourcePrefix="rs-padrin-")
            self.assertGreater(view["curatedCount"], 0)
            rows = view["remote"]
            self.assertLess(len(rows), state["entryCount"])
            self.assertGreater(len({row["titleId"] for row in rows}), 3500)
            self.assertEqual({row["platform"] for row in rows}, {"ps4", "ps5"})
            self.assertTrue(view["sameAddons"])
            self.assertTrue(any(row["gameId"] == service.first_game["gameId"] for row in rows))
            self.assertTrue(any(row["gameId"].startswith("rs-padrino-") and row["cover"] for row in rows))
            self.assertFalse(any(row.get("homebrew") or row.get("archivePassword") or
                                 "zstd" in row["filename"].lower() for row in rows))

    def test_padrino_file_formats_and_zstd_filter(self):
        with tempfile.TemporaryDirectory() as directory, Driver(directory) as service:
            def mapped(filename, **fields):
                link = {"name": "Fixture", "url": "https://example.org/" + filename, **fields}
                entry = {"title": "Example Game", "titleId": "PPSA12345", "downloadLinks": [link]}
                return service.command("map", padrino=True, entry=entry)

            self.assertEqual(mapped("game.exfat")["format"], "exFAT")
            self.assertEqual(mapped("game.ffpfsc")["format"], "FFPFSC")
            archive = mapped("game.7z", archivePassword="example")
            self.assertEqual((archive["compression"], archive["archivePassword"]), ("7z", "example"))
            self.assertEqual(mapped("game-ZSTD.rar"), 1)
            self.assertEqual(mapped("game.rar", format="Zstandard"), 1)

    def test_pegasus_entry_mapping_and_format_filter(self):
        with tempfile.TemporaryDirectory() as directory, Driver(directory) as service:
            valid, unsupported = FIXTURE["packages"]
            mapped = service.command("map", entry=valid)
            self.assertEqual(mapped["sourceId"], "remote")
            self.assertRegex(mapped["id"], r"^rs-evox-c-[0-9a-f]{12}$")
            self.assertEqual(mapped["titleId"], valid["titleId"])
            self.assertEqual(mapped["title"], valid["title"])
            self.assertEqual(mapped["sha256"], valid["checksum"])
            self.assertEqual(mapped["filename"], valid["filename"])
            self.assertEqual(mapped["url"], valid["downloadLinks"][0]["url"])
            self.assertEqual(mapped["format"], "PKG")
            self.assertEqual(mapped["platform"], "ps5")
            self.assertEqual(mapped["sizeBytes"], 0)
            self.assertEqual(service.command("map", entry=unsupported)["format"], "FFPFSC")
            excluded = dict(unsupported, format="ZSTD")
            self.assertEqual(service.command("map", entry=excluded), 1)

            ps4 = dict(valid, category="FPKG", titleId="CUSA01010", sizeBytes=128,
                       filename="example.pkg", downloadSource="https://example.org/release")
            ps4_mapped = service.command("map", entry=ps4)
            self.assertEqual(ps4_mapped["format"], "FPKG")
            self.assertEqual(ps4_mapped["platform"], "ps4")
            self.assertEqual(ps4_mapped["sizeBytes"], 128)
            self.assertEqual(ps4_mapped["browserUrl"], ps4["downloadSource"])

            for suffix in ("rar", "zip"):
                with self.subTest(suffix=suffix):
                    folder = dict(valid, filename=f"game.{suffix}", category="Game folder", extractedBytes=41)
                    self.assertEqual(service.command("map", entry=folder)["compression"], suffix)
            self.assertEqual(service.command("map", entry=dict(valid, filename="game.7z"))["compression"], "7z")
            for errors in (
                {"titleId": "../bad"},
                {"filename": "..\\unsafe.pkg"},
                {"checksum": "xyz"},
                {"sizeBytes": -1},
                {"downloadLinks": [{"url": "http://example.org/test.pkg"}]},
                {"downloadLinks": [{"url": "https://user:secret@example.org/test.pkg"}]},
            ):
                with self.subTest(errors=errors):
                    self.assertEqual(service.command("map", entry=dict(valid, **errors)), -1)

    def test_options_filter_individually_and_resolve_selected_opaque_links(self):
        server = FixtureServer()
        address = server.url.removesuffix("/catalogue.json")
        server.downloads.update({
            "/folder": (200, {"Content-Type": "text/html"}, b'<a href="/game.zip">Download</a>'),
            "/game.zip": (206, {"Content-Type": "application/zip", "Content-Range": "bytes 0-7/29"},
                          b"PK\x03\x04test"),
            "/mirror": (302, {"Location": "/opaque-file"}, b""),
            "/opaque-file": (206, {"Content-Disposition": "attachment; filename=game.pkg",
                                   "Content-Range": "bytes 0-7/42"}, b"\x7fFIHtest"),
            "/unknown": (200, {"Content-Type": "text/html"}, b"<p>No supported link</p>"),
            "/ambiguous": (200, {"Content-Type": "text/html"},
                           b'<a href="/game.zip">ZIP</a><a href="/alt.rar">RAR</a>'),
            "/wrong": (200, {"Content-Disposition": "attachment; filename=bad.zip"}, b"not a zip"),
            "/invalid": (200, {"Content-Disposition": "attachment; filename=game.7z"}, b"not accepted"),
        })
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory, Driver(directory, server.url) as service:
                base = {"title": "Game", "titleId": "PPSA12345", "category": "game"}

                def entry(path, label):
                    return dict(base, downloadLinks=[{"name": label, "url": address + path}])

                for label in ("Folder · LZ4", "FpKg / ZsTd", "FPKG · PFS", "exFAT"):
                    self.assertEqual(service.command("map", entry=entry("/game.zip", label)), 1)
                for suffix in (".rar.001", ".part2.rar"):
                    self.assertEqual(service.command("map", entry=entry("/game" + suffix, "Mirror")), 1)
                direct = service.command("map", entry=entry("/game.zip", "Mirror"))
                self.assertEqual((direct["format"], direct["compression"]), ("Game folder", "zip"))

                for path, label, fmt, filename, size in (
                    ("/folder", "Mirror", "Game folder", "game.zip", 29),
                    ("/mirror", "FPKG · Mirror", "FPKG", "game.pkg", 42),
                ):
                    row = entry(path, label)
                    pending = service.command("map", entry=row)
                    self.assertTrue(pending["remotePage"])
                    self.assertEqual(pending["url"], "")
                    self.assertEqual(pending["remoteLabel"], label)
                    resolved = service.command("resolve", entry=row)
                    self.assertEqual(resolved["id"], pending["id"])
                    self.assertEqual((resolved["format"], resolved["filename"], resolved["sizeBytes"]),
                                     (fmt, filename, size))
                    self.assertEqual(resolved["url"], address + ("/game.zip" if path == "/folder" else path))
                for path in ("/unknown", "/ambiguous", "/wrong", "/invalid"):
                    self.assertEqual(service.command("resolve", entry=entry(path, "Mirror")), -1)

                options = [
                    {"name": "LZ4 · Viki", "url": address + "/game.zip"},
                    {"name": "Folder · Viki", "url": address + "/folder"},
                    {"name": "FPKG · Data", "url": address + "/mirror"},
                ]
                server.feed = {"packages": [dict(base, downloadLinks=options)]}
                self.assertEqual(service.command("start"), 0)
                service.idle()
                rows = service.command("view", sourcePrefix="rs-dlps-")["remote"]
                self.assertCountEqual([row["provider"] for row in rows], ["Folder · Viki", "FPKG · Data"])
                self.assertTrue(all(row["browserAvailable"] and not row["directAvailable"] for row in rows))
                self.assertTrue(all(row["torboxHost"] == "127.0.0.1" for row in rows))
                self.assertEqual(len({row["gameId"] for row in rows}), 1)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_opaque_provider_file_is_identified_by_the_download_get(self):
        server = FixtureServer()
        address = server.url.removesuffix("/catalogue.json")
        html = {"Content-Type": "text/html"}
        sample = b"\x7fFIHtest"
        server.downloads.update({
            "/metadata/opaque": (200, html, b'<title>Provider</title>'
                                 b'<a HREF = "/token" download="Game &amp; Add-on">Download</a>'),
            "/token": (206, {"Content-Range": "bytes 0-7/42",
                             "Content-Disposition": "attachment; filename*=UTF-8''Game%20%26%20Add-on"},
                       sample),
            "/metadata/full": (200, html, b"<a href=/no-range download='Full Game.pkg'>Download</a>"),
            "/no-range": (200, {"Content-Type": "application/octet-stream"}, sample + b"rest"),
            "/metadata/plain": (200, html, b'<h1>Provider Game.pkg</h1><a href="/plain">Download</a>'),
            "/bad-type": (200, html, b'<a href="/token" download="Game.zip">Download</a>'),
            "/bad-magic": (200, html, b'<a href="/plain" download="Game.zip">Download</a>'),
            "/plain": (200, {}, sample),
            "/unsupported": (200, html, b'<a href="/plain" download="Game.7z">Download</a>'),
        })
        server.feed = {"packages": [{"title": "Game", "titleId": "PPSA12345", "format": "Unknown file type",
                        "downloadLinks": [{"name": label, "url": address + "/metadata/" + label}
                                          for label in ("opaque", "full")]}]}
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory, Driver(directory, server.url) as service:
                self.assertEqual(service.command("start"), 0)
                service.idle()

                rows = service.command("view", sourcePrefix="rs-dlps-")["remote"]
                ids = ",".join(row["id"] for row in rows)
                self.assertTrue(all(row["browserAvailable"] and row["format"] == "Unknown" for row in rows))
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    result = service.command("metadata", ids=ids)
                    if not result["pending"]:
                        break
                    time.sleep(0.02)
                self.assertFalse(result["pending"])
                files = {row["filename"]: row for row in result["files"]}
                self.assertEqual(files["Full Game.pkg"]["format"], "PKG")
                self.assertNotIn("format", files["Game & Add-on"])
                self.assertTrue(all(row["sizeBytes"] == 0 for row in files.values()))
                self.assertNotIn("/token", server.paths)
                self.assertNotIn("/no-range", server.paths)

                base = server.feed["packages"][0]

                def resolve(path):
                    return service.command("resolve", entry=dict(
                        base, downloadLinks=[{"name": "Mirror", "url": address + path}]))

                for path, name, size in (("/metadata/opaque", "Game & Add-on", 42),
                                         ("/metadata/full", "Full Game.pkg", 12),
                                         ("/metadata/plain", "Provider Game.pkg", 8)):
                    with self.subTest(path=path):
                        resolved = resolve(path)
                        self.assertEqual((resolved["filename"], resolved["format"], resolved["sizeBytes"]),
                                         (name, "PKG", size))
                self.assertIn(("/token", "bytes=0-7"), server.ranges)
                self.assertIn(("/no-range", "bytes=0-7"), server.ranges)
                for path in ("/bad-type", "/bad-magic", "/unsupported"):
                    with self.subTest(path=path):
                        self.assertEqual(resolve(path), -1)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_provider_sizes_are_available_without_changing_transfer_length(self):
        server = FixtureServer()
        address = server.url.removesuffix("/catalogue.json")
        html = {"Content-Type": "text/html"}
        server.downloads.update({
            "/metadata/akira": (200, html, b'<meta property="og:title" content="game.rar">'
                                b'<meta property="og:description" content="RAR file &nbsp; 1.48 GB">'),
            "/metadata/viking": (200, html, b'<img src="' + b'a' * 660_000 +
                                 b'"><h2 id="filename">game.pkg</h2><p id="size">42.25 GiB</p>'),
            "/metadata/data": (200, html, b'<h1>game.pkg</h1>'
                               b'<span data-size-bytes="43123456789">43.1 GB</span>'),
            "/metadata/unavailable": (404, html, b'Unavailable'),
        })
        server.feed = {"packages": [{"title": "Game", "titleId": "PPSA12345", "sizeBytes": 1000000,
                        "downloadLinks": [{"name": label, "url": address + "/metadata/" + label}
                                          for label in ("akira", "viking", "data", "unavailable")]}]}
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory, Driver(directory, server.url) as service:
                self.assertEqual(service.command("start"), 0)
                service.idle()
                rows = service.command("view", sourcePrefix="rs-dlps-")["remote"]
                self.assertEqual(len(rows), 4)
                for row in rows:
                    self.assertTrue(row["providerMetadata"])
                    self.assertEqual(row["sizeBytes"], 0)
                    self.assertEqual(row["sizeEstimateBytes"], 1000000)
                ids = ",".join(row["id"] for row in rows)
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    result = service.command("metadata", ids=ids)
                    if not result["pending"]:
                        break
                    time.sleep(0.02)
                self.assertFalse(result["pending"])
                self.assertCountEqual(
                    [(row["sizeBytes"], row["exact"]) for row in result["files"] if row["status"] == "found"],
                    [(1480000000, False), (int(42.25 * 1024 ** 3), False), (43123456789, True)],
                )
                status = {row["id"]: row["status"] for row in result["files"]}
                self.assertEqual(status[next(row["id"] for row in rows if row["provider"] == "unavailable")],
                                 "missing")
                self.assertEqual(service.command("metadata", ids=ids), result)
                self.assertEqual(sum(path.startswith("/metadata/") for path in server.paths), 4)
                self.assertEqual({row["status"] for row in result["options"]}, {"found", "missing"})
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_provider_layouts_and_confirmed_deletions(self):
        server = FixtureServer()
        address = server.url.removesuffix("/catalogue.json")
        html = {"Content-Type": "text/html"}
        pages = {
            "akira": (200, b'<title>Fixture.rar \xc2\xb7 AkiraBox</title>'
                           b'<meta name="description" content="RAR file \xc2\xb7 118.40 GB \xe2\x80\x94 download it free on AkiraBox.">'),
            "1fichier": (200, b'<div><span id="file-title" class="heading tier-name">Fixture.fpkg</span>'
                              b'<span class="tier-feat"><strong>127.13 GB</strong></span></div>'),
            "rapidgator": (200, b'<title>Download Fixture.rar - Rapidgator</title>'
                                b'<p>Maximum file size: <strong>900 TB</strong></p>'
                                b'<table><tr><td>File size:</td><td><strong>118.4 GB</strong></td></tr></table>'),
            "buzz": (200, b'<h1>Fixture.zip</h1><button><span>Download File</span>'
                          b'<strong>(3.75 GiB)</strong></button>'),
            "buzz-sibling": (200, b'<h1>Fixture.zip</h1><div><button>Download File</button>'
                                  b'<span><strong>42 MiB</strong></span></div>'),
            "data": (200, b'<h1>Fixture.pkg</h1><span id="filesize">1 GB</span>'
                           b'<span data-file-size="1323456789"></span>'),
            "other": (200, b'<title>Fixture.zip</title><div data-size="99999999999">Premium plan</div>'
                            b'<p>Upload limit: 10 TB</p><p>Page 24</p><p>Cloud storage 50 GB</p>'
                            b'<span data-size-bytes="4321000000">Premium quota</span>'
                            b'<script>var plans = {"sizeBytes": 245000000};</script>'),
            "keeper": (200, b'<h1>Fixture.rar</h1><p>File <span>size:</span> <b>71 KiB</b></p>'),
            "mediafire": (200, b'<div class="filename">Fixture.zip</div><p><span>File size:</span> '
                                  b'<strong>13.04 MB</strong></p>'),
            "unsupported-title": (200, b'<title>Fixture.rar - ZSTD</title>'),
            "gofile": (200, b'<h1>Fixture.pkg</h1><script type="application/ld+json">'
                               b'{"name":"Fixture.pkg","contentSize":"345678901"}</script>'),
            "bytes": (200, b'<p>File size: <b>123 B</b></p>'),
            "kb": (200, b'<p>File size: <b>1.2 KB</b></p>'),
            "tb": (200, b'<p>File size: <b>1.2 TB</b></p>'),
            "tib": (200, b'<p>File size: <b>1.5 TiB</b></p>'),
            "viking": (404, b'<title>VikingFile</title><h1 id="filename">File not found</h1>'),
            "no-type": (410, b'<h1>File removed</h1>'),
            "not-found": (200, b'<h1>Fixture.pkg</h1><p>FiLe<br><b>not</b> \n\t found</p>'),
            "deleted": (200, b'<h1 class="filename"> \n DeLeTeD \t</h1><p>File size: 118 GB</p>'),
            "removed": (200, b'<title>Fixture.rar</title><p>  FiLe&nbsp; <b>removed</b> </p>'),
            "data-passive-cf": (404, b'<h1>File not found</h1><script src="/cdn-cgi/challenge-platform/scripts/jsd/main.js"></script>'),
            "archive-404": (404, b'<h1>This item is no longer available.</h1>'),
            "challenge": (403, b'<title>Just a moment...</title><p>File not found</p>'),
            "challenge-ok": (200, b'<script>window._cf_chl_opt = {};</script><h1>File removed</h1>'),
            "provider-error": (503, b'<h1>File not found</h1>'),
            "fake-name": (200, b'<script>const message = "File not found";</script>'
                               b'<span data-filename="Deleted"></span>'),
            "live": (200, b'<title>Mirror.pkg</title><p>File size: 1.2 MB</p>'),
            "rootz/active": (200, b'<script>const shortId = "active"; const pageToken = "rootz-fixture-token";</script>'
                                  b'<span data-file-size="9 GB"></span>'),
            "rootz/dead": (200, b'<script>var data = {"shortId":"dead","pageToken":"rootz-fixture-token"};</script>'
                                b'<title>Former File.rar</title>'),
        }
        for key, (status, body) in pages.items():
            server.downloads["/metadata/" + key] = (status, {} if key == "no-type" else html, body)
        for key in ("active", "dead"):
            obj = {"fileName": "Fixture.rar", "size": 118401234567, "mimeType": "application/vnd.rar",
                   "status": "active" if key == "active" else "deleted", "downloadAllowed": key == "active"}
            server.downloads[f"/api/files/download-by-short?shortId={key}"] = (
                200, {"Content-Type": "application/json"}, json.dumps(obj).encode())
        server.feed = {"packages": [{"title": "Metadata fixture", "titleId": "PPSA12345",
                        "downloadLinks": [{"name": key, "url": address + "/metadata/" + key} for key in pages]}]}
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory, Driver(directory, server.url) as service:
                self.assertEqual(service.command("start"), 0)
                service.idle()
                rows = {row["provider"]: row["id"] for row in service.command("view", sourcePrefix="rs-dlps-")["remote"]}

                def metadata(keys):
                    ids = ",".join(rows[key] for key in keys)
                    deadline = time.monotonic() + 8
                    while time.monotonic() < deadline:
                        result = service.command("metadata", ids=ids)
                        if not result["pending"]:
                            return {row["id"]: row for row in result["files"]}
                        time.sleep(0.02)
                    self.fail("Metadata worker did not complete")

                results = metadata(pages)
                self.assertEqual(len(results), len(pages))
                for key, filename, fmt, compression, size, exact in (
                    ("akira", "Fixture.rar", "Game folder", "rar", 118400000000, False),
                    ("1fichier", "Fixture.fpkg", "FPKG", None, 127130000000, False),
                    ("rapidgator", "Fixture.rar", "Game folder", "rar", 118400000000, False),
                    ("buzz", "Fixture.zip", "Game folder", "zip", int(3.75 * 1024 ** 3), False),
                    ("buzz-sibling", "Fixture.zip", "Game folder", "zip", 42 * 1024 ** 2, False),
                    ("data", "Fixture.pkg", "PKG", None, 1323456789, True),
                    ("keeper", "Fixture.rar", "Game folder", "rar", 71 * 1024, False),
                    ("rootz/active", "Fixture.rar", "Game folder", "rar", 118401234567, True),
                    ("other", "Fixture.zip", "Game folder", "zip", 0, False),
                    ("mediafire", "Fixture.zip", "Game folder", "zip", 13040000, False),
                    ("gofile", "Fixture.pkg", "PKG", None, 345678901, True),
                ):
                    with self.subTest(page=key):
                        row = results[rows[key]]
                        self.assertEqual(row["status"], "found" if size else "parse-no-size")
                        self.assertEqual((row["filename"], row["format"], row.get("compression"),
                                          row["sizeBytes"], row["exact"]), (filename, fmt, compression, size, exact))
                for key in ("viking", "no-type", "not-found", "deleted", "removed", "rootz/dead",
                            "challenge", "challenge-ok", "provider-error", "fake-name", "unsupported-title", "data-passive-cf", "archive-404"):
                    with self.subTest(page=key):
                        row = results[rows[key]]
                        expected = {"challenge": "challenge", "challenge-ok": "challenge",
                                    "provider-error": "http-error", "fake-name": "parse-no-size",
                                    "unsupported-title": "unsupported-format"}.get(key, "missing")
                        self.assertEqual(row["status"], expected)
                        self.assertNotIn("filename", row)
                        self.assertEqual(row["sizeBytes"], 0)
                for key, size in (("bytes", 123), ("kb", 1200), ("tb", 1200000000000),
                                  ("tib", int(1.5 * 1024 ** 4))):
                    self.assertEqual((results[rows[key]]["sizeBytes"], results[rows[key]]["exact"]),
                                     (size, False))
                self.assertCountEqual(server.page_tokens, [
                    (f"/api/files/download-by-short?shortId={key}", "rootz-fixture-token")
                    for key in ("active", "dead")
                ])
                by_id = {row["id"]: row for row in service.command("view")["remote"]}
                self.assertNotIn(rows["viking"], by_id)
                self.assertNotIn("providerMissing", by_id[rows["challenge"]])

                self.assertTrue(service.command("mirror-fixture", ids=[rows["viking"], rows["live"]]))
                choices = {row["id"]: row for row in service.command("view")["remote"]}
                self.assertNotIn(rows["viking"], choices)
                self.assertNotIn("providerMissing", choices[rows["live"]])
                metadata_row = metadata(["viking"])[rows["viking"]]
                self.assertEqual((metadata_row["status"], metadata_row["mirrorCount"], metadata_row["filename"]),
                                 ("found", 1, "Mirror.pkg"))
                self.assertEqual(server.paths.count("/metadata/viking"), 1)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_details_recovers_overflow_without_starving_another_title(self):
        server = FixtureServer()
        base = server.url.removesuffix("/catalogue.json")
        first = {"title": "Crowded fixture", "titleId": "PPSA98765"}
        other = {"title": "Overflow fixture", "titleId": "PPSA98766"}
        server.feed = {"packages": [
            *[dict(first, downloadLinks=[{"url": base + f"/crowd-{i}.pkg", "name": f"Choice {i}"}])
              for i in range(2050)],
            dict(other, downloadLinks=[{"url": base + "/overflow-a.pkg", "name": "Overflow A"},
                                       {"url": base + "/overflow-b.pkg", "name": "Overflow B"}]),
        ]}
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory, Driver(directory, server.url) as service:
                self.assertEqual(service.command("start"), 0)
                service.idle()
                rows = service.command("view", sourcePrefix="rs-dlps-")["remote"]
                overflow = [row for row in rows if row["titleId"] == other["titleId"]]
                self.assertEqual([row["provider"] for row in overflow], ["Overflow A"])
                details = service.command("title-options", id=overflow[0]["id"])
                self.assertEqual({row["provider"] for row in details["releases"]}, {"Overflow B"})
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_direct_metadata_range_preserves_exact_length(self):
        server = FixtureServer()
        base = server.url.removesuffix("/catalogue.json")
        server.feed = {"packages": [{"title": "Direct fixture", "titleId": "CUSA98765",
                        "downloadLinks": [{"url": base + "/Direct.pkg", "name": "Direct"}]}]}
        server.downloads["/Direct.pkg"] = (206, {"Content-Type": "application/octet-stream",
                                               "Content-Range": "bytes 0-7/2000"}, b"\x7fFIHtest")
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory, Driver(directory, server.url) as service:
                self.assertEqual(service.command("start"), 0)
                service.idle()
                row, = service.command("view", sourcePrefix="rs-dlps-")["remote"]
                self.assertTrue(row["providerMetadata"])
                deadline = time.monotonic() + 5
                while True:
                    result = service.command("metadata", ids=row["id"])
                    if not result["pending"] or time.monotonic() >= deadline:
                        break
                    time.sleep(.02)
                file, = result["files"]
                self.assertEqual((file["status"], file["sizeBytes"], file["exact"]), ("found", 2000, True))
                self.assertIn(("/Direct.pkg", "bytes=0-7"), server.ranges)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_duplicate_mirrors_keep_variants_and_reserve_alternates(self):
        def link(label, url, **fields):
            return {"name": label, "url": url, **fields}

        def package(title_id, *links):
            return {"title": title_id, "titleId": title_id, "downloadLinks": links}

        def akira(i, label="EUR 12.xx Base", **fields):
            return link(f"{label} - AkiraBox", f"https://akirabox.to/{i:012d}/file", **fields)

        def viki(i, label="EUR 12.xx Base", **fields):
            return link(f"{label} · Viki", f"https://vikingfile.com/f/{i:010d}", **fields)

        hash_value = "a" * 64
        fixtures = [
            package("PPSA00001", akira(1), viki(1), akira(2, "EUR 12.xx DLC")),
            package("PPSA00002", viki(2), viki(3), akira(3)),
            package("PPSA00003", akira(4, sizeBytes=100), viki(4, sizeBytes=200)),
            package("PPSA00004", akira(5, "Game + DLC"), viki(5, "Game - DLC")),
            package("PPSA00005", akira(6, part=1), viki(6, part=1)),
            package("PPSA00006", akira(7)),
            package("PPSA00006", viki(7)),
            package("PPSA00007", link("FPKG", "https://one.example/app.pkg", sha256=hash_value, sizeBytes=100),
                    link("FPKG", "https://two.example/app.pkg", sha256=hash_value, sizeBytes=100),
                    link("FPKG", "https://three.example/app.pkg", sha256=hash_value, sizeBytes=200)),
        ]
        fixtures.extend(package(f"PPSA{i:05d}", akira(i), viki(i)) for i in range(10000, 11025))
        server = FixtureServer()
        server.feed = {"packages": fixtures}
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory, Driver(directory, server.url) as service:
                self.assertEqual(service.command("start"), 0)
                service.idle()
                rows = service.command("view", sourcePrefix="rs-dlps-")["remote"]
                by_title = {}
                for row in rows:
                    self.assertNotIn("mirrorKey", row)
                    by_title.setdefault(row["titleId"], []).append(row)
                self.assertEqual([len(by_title.get(f"PPSA{i:05d}", [])) for i in range(1, 8)], [2, 3, 2, 2, 0, 2, 2])
                base = next(row for row in by_title["PPSA00001"] if row.get("mirrorCount"))
                self.assertEqual(base["mirrorCount"], 2)
                self.assertCountEqual(base["torboxHosts"], ["akirabox.to", "vikingfile.com"])
                self.assertTrue(all(by_title[f"PPSA{i:05d}"][0]["mirrorCount"] == 2 for i in range(10000, 11025)))
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_legacy_source_preferences_do_not_control_the_built_in_catalogues(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "state.json").write_text(json.dumps({
                "schema": 8,
                "sources": {"enabled": 0, "noticeVersion": 1, "acknowledgedAt": 1},
            }))
            metadata = root / "remote-sources-v1.json"
            custom_id = "custom-0123456789abcdef"
            metadata.write_text(json.dumps({
                "schemaVersion": 1,
                "revision": 4,
                "onboardingRevision": 1,
                "sources": [
                    {"id": "dlps", "name": "DLPS", "url": "https://pegasus-dl.dev/catalogs/dlps.json",
                     "enabled": False, "builtIn": True},
                    {"id": "padrino", "name": "Padrino", "url": "https://github.com/B4ckd0orl/Padrino-Store",
                     "enabled": False, "builtIn": True},
                    {"id": custom_id, "name": "Custom", "url": "https://example.org/custom.json",
                     "enabled": True, "builtIn": False},
                ],
            }))
            cache = root / "remote-cache"
            cache.mkdir()
            for source_id in ("dlps", custom_id):
                (cache / f"{source_id}.json").write_text(json.dumps({"packages": FIXTURE["packages"][:1]}))
            for corrupt in (False, True):
                if corrupt:
                    metadata.write_text("{broken")
                with Driver(directory) as service:
                    status = service.command("selection")
                    self.assertTrue(status["acknowledged"])
                    self.assertCountEqual(status["enabled"], ["archive", "vikingfile", "akirabox", "datanodes", "remote"])
                    snapshot = service.command("show")
                    self.assertEqual({s["id"] for s in snapshot["sources"]}, DEFAULTS)
                    self.assertGreater(source(snapshot, "padrino")["entryCount"], 10000)
                    view = service.command("view", sourcePrefix="rs-dlps-")
                    self.assertGreater(view["curatedCount"], 0)
                    self.assertEqual(len(view["remote"]), 1)
                    self.assertFalse(service.command("view", sourcePrefix="rs-custom-")["remote"])

    def test_pspkg_general_sources_exclude_arabic_and_keep_platforms_separate(self):
        def package(identity, category, page, title_id=""):
            return {"id": identity, "title": identity, "titleId": title_id,
                    "category": category,
                    "description": f"Public listing.\nSource: https://pspkg.com/{page}\nFirmware compatibility not verified.",
                    "downloadLinks": [{"name": "PKG", "url": f"https://example.org/{identity}.pkg"}]}

        ps5 = package("pspkg-012345abcdef", "PS5", "ps5/a-game-123.html")
        ps4 = package("pspkg-abcdef012345", "PS4", "ps4/another-game-456.html", "CUSA12345")
        arabic = package("pspkg-arabic-77e84b9af10e1f00", "PS5", "arabic-games/arabic-789.html")
        mislabeled = package("pspkg-fedcba987654", "PS5", "arabic-games/other-789.html")
        feed = {"packages": [ps5, ps4, arabic, mislabeled]}
        with tempfile.TemporaryDirectory() as directory, Driver(directory) as service:
            manifest = service.command("show")
            for source_id, platform, expected in (("pspkg-ps5", "ps5", "P5012345abcdef"),
                                                  ("pspkg-ps4", "ps4", "CUSA12345")):
                built_in = source(manifest, source_id)
                self.assertEqual(built_in["url"], f"https://pspkg.com/{platform}/")
                parsed = service.command("parse", source=source_id, feed=feed)
                self.assertEqual((parsed["code"], parsed["skipped"]), (0, 0))
                self.assertEqual(len(parsed["releases"]), 1)
                self.assertEqual(parsed["releases"][0]["titleId"], expected)
                self.assertEqual(parsed["releases"][0]["platform"], platform)
            self.assertEqual(service.command("parse", source="pspkg-ps5",
                                             feed={"packages": [arabic]})["releases"], [])

    def test_pspkg_links_keep_prefilter_edition_and_part_diagnostics(self):
        html = ('<h3>Game PKG PPSA12345 v2.31 EUR 54.25 GB</h3>'
                '<a href="https://vikingfile.com/f/game123456">VikingFile</a>'
                '<a href="https://rootz.so/d/mirror">Rootz</a>'
                '<h3>Game PKG ZSTD</h3><a href="https://datanodes.to/abcd12345678">Download</a>'
                '<h3>Game PKG</h3><a href="https://filekeeper.net/abcd12345678">Part 1</a>'
                '<h3>Update PKG</h3><a href="https://rootz.so/d/update">Update</a>')
        with tempfile.TemporaryDirectory() as directory, Driver(directory) as service:
            links = service.command("pspkg-links", html=html)
            self.assertEqual([row["filterReason"] for row in links],
                             ["", "", "unsupported-format", "multipart", "unsupported-category"])
            self.assertEqual(links[0]["titleId"], "PPSA12345")
            self.assertEqual(links[0]["sizeEstimateBytes"], 54_250_000_000)
            self.assertIn("ZSTD", links[2]["edition"])

    def test_pspkg_handoff_accepts_provider_pages_or_verifiable_file_candidates(self):
        with tempfile.TemporaryDirectory() as directory, Driver(directory) as service:
            for url, expected in (
                ("https://vik1ngf1le.online/f/54VHFcPHGM", True),
                ("https://vikingfile.com/f/not-a-file", False),
                ("https://files.example.org/game.pkg?token=abc", True),
                ("https://pspkg.com/ps5/game.pkg", False),
                ("https://files.example.org/landing", False),
            ):
                self.assertEqual(service.command("pspkg-candidate", url=url), expected, url)

    def test_pspkg_job_persists_stable_page_and_separate_provider(self):
        url = "https://pspkg.com/ps5/download-cyberpunk-fixture-123.html"
        entry = {"id": "pspkg-012345abcdef", "category": "PS5", "title": "Cyberpunk 2077",
                 "description": "Public PSPKG index.\nSource: https://pspkg.com/ps5/cyberpunk-fixture-123.html\n",
                 "downloadLinks": [{"url": url, "name": "PSPKG", "page": True}]}
        provider = "https://buzzheavier.com/fixture12345"
        with tempfile.TemporaryDirectory() as directory, Driver(directory) as service:
            saved = service.command("pspkg-job-page", source="pspkg-ps5", entry=entry, url=provider)
            self.assertTrue(saved["valid"])
            self.assertEqual((saved["browserUrl"], saved["providerPage"], saved["torboxUrl"]),
                             (url, provider, provider))
            self.assertFalse(service.command("pspkg-job-page", source="pspkg-ps5", entry=entry,
                                             url=provider + "?private=secret")["valid"])

    def test_pspkg_indexes_discover_older_games_and_keep_platforms_separate(self):
        server = FixtureServer()
        html = {"Content-Type": "text/html"}
        for platform in ("ps5", "ps4"):
            title = "Cyberpunk 2077 Ultimate Edition" if platform == "ps5" else "Cyberpunk 2077"
            entries = "".join(f'<a href="/{platform}/old-game-{i}.html">{i} Game {i} 2026/01/02</a>'
                              for i in range(1, 270))
            entries += (f'<a href="/{platform}/cyberpunk-fixture-999.html"><span class="gl-n">270</span>'
                        f'<span class="gl-t" dir="auto">{title}</span><span class="gl-d">2025/11/22</span></a>'
                        '<a href="/arabic-games/arabic-game-456.html">Arabic</a>'
                        '<a href="/ps3/not-this-game-456.html">Other platform</a>')
            server.downloads[f"/{platform}/{platform}-games-list"] = (200, html, entries.encode())
            response = (b'<h2>Game PKG</h2><p>Awaiting verification</p>' if platform == "ps5" else
                        b'<h2>Game FPKG</h2><p>Links will appear here.</p>')
            server.downloads[f"/{platform}/download-cyberpunk-fixture-999.html"] = (200, html, response)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory, Driver(directory) as service:
                for platform, status in (("ps5", "interactive-verification"), ("ps4", "parse-no-size")):
                    base = server.url.replace("/catalogue.json", f"/{platform}/")
                    result = service.command("pspkg-scan", source=f"pspkg-{platform}", url=base, gameLimit=400)
                    self.assertEqual((result["error"], result["discovered"], result["truncated"]), ("", 270, False))
                    self.assertEqual(server.paths[-1], f"/{platform}/{platform}-games-list")
                    packages = result["feed"]["packages"]
                    self.assertTrue(all(row["category"] == platform.upper() for row in packages))
                    game = packages[-1]
                    self.assertEqual(game["title"],
                                     "Cyberpunk 2077 Ultimate Edition" if platform == "ps5" else "Cyberpunk 2077")
                    self.assertNotIn("2025", game["title"])
                    url = game["downloadLinks"][0]["url"]
                    self.assertEqual(url, base + "download-cyberpunk-fixture-999.html")
                    self.assertEqual(len(service.command("parse", source=f"pspkg-{platform}",
                                                      feed={"packages": [game]})["releases"]), 1)
                    metadata = service.command("metadata-page", url=url)
                    self.assertEqual(metadata["status"], status)
                    self.assertEqual(metadata["links"], [])
                    capped = service.command("pspkg-scan", source=f"pspkg-{platform}", url=base, gameLimit=8)
                    self.assertTrue(capped["truncated"])
                    self.assertEqual(capped["discovered"], 8)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_provider_forms_and_consistent_cyberpunk_mapping(self):
        fixture = json.loads((ROOT / "tests/fixtures/community-provider-pages.json").read_text())
        with tempfile.TemporaryDirectory() as directory, Driver(directory) as service:
            for source_id, indexes in fixture["sources"].items():
                for index in indexes:
                    provider, url = fixture["providers"][index]
                    with self.subTest(source=source_id, provider=provider, url=url):
                        self.assertTrue(service.command("provider-page", url=url))
                        if source_id.startswith("pspkg-"):
                            continue
                        entry = {"title": "Cyberpunk 2077", "titleId": "PPSA12345",
                                 "downloadLinks": [{"name": provider, "url": url, "page": True}]}
                        generic = service.command("map", entry=entry)
                        padrino = service.command("map", entry=entry, padrino=True)
                        self.assertEqual(generic["browserUrl"], padrino["browserUrl"])
                        self.assertTrue(generic["remotePage"])
            for url in (
                'https://buzzheavier.com.evil.test/abc12345', 'https://user@buzzheavier.com/abc12345',
                'https://buzzheavier.com:443/abc12345', 'https://buzzheavier.com/abc12345#',
                'https://filekeeper.net/../abc12345', 'https://datanodes.to/abc12345\\danger',
                'https://datanodes.to/abc12345\n', 'https://akirabox.com/short/file',
                'https://rapidgator.net/file/0123456789abcdef0123456789abcdef/%2Funsafe.html',
                'https://buzzheavier.com/' + 'a' * 2048,
            ):
                self.assertEqual(service.command("provider-page", url=url), "", url)
            url = fixture["providers"][9][1]
            for fields, reason in (
                ({"label": "Game part 1"}, "multipart"), ({"volume": 1}, "multipart"),
                ({"label": "Game - LZ4"}, "unsupported-format"), ({"compression": "ZSTD"}, "unsupported-format"),
            ):
                entry = {"title": "Cyberpunk 2077", "titleId": "PPSA12345",
                         "downloadLinks": [{"url": url, **fields}]}
                for source_id in ("dlps", "padrino", "pippo-exfat", "pfs"):
                    row = service.command("replay", source=source_id, entry=entry)["links"][0]
                    self.assertEqual((row["rawUrl"], row["filterReason"]), (url, reason))
            entry = {"title": "Cyberpunk 2077", "titleId": "PPSA12345",
                     "downloadLinks": [{"url": url, "label": "exFAT", "version": "2.31", "region": "EUR"}]}
            for padrino in (True, False):
                mapped = service.command("map", entry=entry, padrino=padrino)
                self.assertEqual((mapped["format"], mapped["version"], mapped["region"]), ("exFAT", "2.31", "EUR"))

    def test_html_size_fixtures_cover_provider_markup_units_and_failures(self):
        server = FixtureServer()
        base = server.url.removesuffix("/catalogue.json")
        html = {"Content-Type": "text/html"}
        cases = {
            "buzz": ('<div class="size">1.48 GB</div>', 1_480_000_000),
            "akira": ('<h1>Game.pkg</h1><span>1,25 GiB</span><aside class="premium-plan"><span class="size">2 TB</span></aside>', 1_342_177_280),
            "fichier": ('<span class="tier-feat">100 MB</span>', 100_000_000),
            "rootz": ('<main>23.5 gb</main>', 23_500_000_000),
            "rapid": ('<p>File size: <b>1.5 TiB</b></p><p>Storage quota: 1 TB</p>', 1_649_267_441_664),
            "data": ('<p>1.5 tb</p>', 1_500_000_000_000),
            "filekeeper": ('<span>5 MiB</span>', 5_242_880),
            "datavaults": ('<p>8.5 mB</p>', 8_500_000),
        }
        for name, (body, _) in cases.items():
            server.downloads[f"/metadata/{name}"] = (200, html, body.encode())
        server.downloads.update({
            "/metadata/ambiguous": (200, html, b"<p>20 GB</p><p>40 GB</p>"),
            "/metadata/exact": (200, html, b'<span data-size-bytes="54321">54.2 MB</span>'),
            "/metadata/challenge": (403, {**html, "cf-mitigated": "challenge"}, b"File not found"),
            "/metadata/missing": (404, html, b"Not here"),
            "/metadata/viking-deleted": (200, html, b"<h1>File&nbsp;<strong>not found</strong>.</h1>"),
            "/metadata/fichier-deleted": (200, html, b"<div>FILE DELETED</div><button disabled>Download</button>"),
            "/metadata/data-deleted": (200, {"Content-Type": "text/plain"}, b"File missing"),
            "/metadata/json-deleted": (200, {"Content-Type": "application/json"}, b'{"message":"File missing"}'),
            "/metadata/with-size": (200, html, b"<p>File not found</p><p>1.234 GB</p>"),
            "/metadata/with-button": (200, html, b'<p>File deleted</p><button>Download file</button>'),
            "/metadata/with-link": (200, html, b'<p>File missing</p><a href="/get" download>Get file</a>'),
            "/metadata/hidden-button": (200, html, b'<p>File missing</p><nav><a href="/app">Download</a></nav>'),
            "/metadata/script-only": (200, html, b'<script>const error = "File missing";</script>'),
            "/metadata/retry": (503, html, b"<h1>File not found</h1>"),
            "/metadata/redirect": (302, {"Location": "http://127.0.0.1:1/private"}, b""),
            "/metadata/too-large": (200, html, b"<body>" + b"x" * (1024 * 1024)),
        })
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory, Driver(directory) as service:
                for name, (_, expected) in cases.items():
                    result = service.command("metadata-page", url=f"{base}/metadata/{name}")
                    self.assertEqual(result, {"status": "found", "exact": False, "sizeBytes": expected}, name)
                self.assertEqual(service.command("metadata-page", url=f"{base}/metadata/exact"),
                                 {"status": "found", "exact": True, "sizeBytes": 54321})
                for name, status in (("ambiguous", "ambiguous-size"), ("challenge", "challenge"),
                                     ("missing", "missing"), ("viking-deleted", "missing"),
                                     ("fichier-deleted", "missing"), ("data-deleted", "missing"),
                                     ("json-deleted", "missing"), ("hidden-button", "missing"),
                                     ("with-size", "found"), ("with-button", "parse-no-size"),
                                     ("with-link", "parse-no-size"), ("script-only", "parse-no-size"),
                                     ("retry", "http-error"), ("redirect", "redirected-to-invalid-host"),
                                     ("too-large", "page-too-large")):
                    self.assertEqual(service.command("metadata-page", url=f"{base}/metadata/{name}")["status"], status)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_buzzheavier_recovery_notice_marks_the_file_missing(self):
        server = FixtureServer()
        base = server.url.removesuffix("/catalogue.json")
        html = {"Content-Type": "text/html"}
        deleted = "/pspkg-provider/buzzheavier.com/deleted"
        live = "/pspkg-provider/buzzheavier.com/live"
        other = "/metadata/other"
        notice = (b"<p>Try <strong>recovering this file</strong>.</p>"
                  b"<p>File size: 1 MB</p><button>Download file</button>")
        server.downloads.update({
            deleted: (200, html, notice),
            live: (200, html, b"<h1>Try recovering this file.pkg</h1><p>File size: 1 MB</p>"),
            other: (200, html, notice),
        })
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory, Driver(directory) as service:
                url = base + deleted
                self.assertEqual(service.command("metadata-page", url=url),
                                 {"status": "missing", "sizeBytes": 0, "exact": False})
                self.assertEqual(service.command("resolve", entry={
                    "title": "BuzzHeavier fixture", "titleId": "PPSA12345",
                    "downloadLinks": [{"name": "BuzzHeavier", "url": url}],
                }), -2)
                for path in (live, other):
                    with self.subTest(path=path):
                        self.assertEqual(service.command("metadata-page", url=base + path),
                                         {"status": "found", "sizeBytes": 1_000_000, "exact": False})
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_metadata_batches_cover_all_releases_on_selected_title(self):
        server = FixtureServer()
        base = server.url.removesuffix("/catalogue.json")
        links = []
        for i in range(42):
            path = f"/metadata/option-{i}"
            links.append({"name": f"Option {i}", "url": base + path})
            server.downloads[path] = (200, {"Content-Type": "text/html"}, b"<p>File size: 1 GB</p>")
        server.feed = {"packages": [{"title": "Cyberpunk 2077", "titleId": "PPSA12345", "downloadLinks": links}]}
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory, Driver(directory, server.url) as service:
                self.assertEqual(service.command("start"), 0)
                service.idle()
                rows = service.command("view", sourcePrefix="rs-dlps-")["remote"]
                self.assertEqual(len(rows), 42)
                self.assertFalse(any(path.startswith("/metadata/") for path in server.paths))
                for start in range(0, len(rows), 32):
                    batch = rows[start:start + 32]
                    ids = ",".join(row["id"] for row in batch)
                    deadline = time.monotonic() + 8
                    while True:
                        result = service.command("metadata", ids=ids)
                        if not result["pending"] or time.monotonic() >= deadline:
                            break
                        time.sleep(.02)
                    self.assertFalse(result["pending"])
                    self.assertEqual(len(result["files"]), len(batch))
                    self.assertEqual({row["status"] for row in result["options"]}, {"found"})
                self.assertCountEqual([path for path in server.paths if path.startswith("/metadata/")],
                                      [f"/metadata/option-{i}" for i in range(42)])
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_missing_links_disappear_from_the_catalogue_without_removing_good_mirrors(self):
        server = FixtureServer()
        base = server.url.removesuffix("/catalogue.json")
        viking = "/metadata/vikingfile.com/game"
        akira = "/metadata/akirabox.com/game"
        singles = "/metadata/filekeeper.net/other"
        server.feed = {"packages": [{
            "title": "Cyberpunk 2077", "titleId": "PPSA12345",
            "downloadLinks": [
                {"url": base + viking, "name": "Game - VikingFile"},
                {"url": base + akira, "name": "Game - AkiraBox"},
                {"url": base + singles, "name": "Alternate edition - FileKeeper"},
            ],
        }]}
        html = {"Content-Type": "text/html"}
        server.downloads.update({
            viking: (200, html, b"<h1>File not found</h1>"),
            akira: (200, html, b"<h1>File missing</h1><p>1.25 GB</p>"),
            singles: (200, html, b"<h1>File deleted</h1>"),
        })
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory, Driver(directory, server.url) as service:
                self.assertEqual(service.command("start"), 0)
                service.idle()
                rows = service.command("view", sourcePrefix="rs-dlps-")["remote"]
                self.assertEqual(len(rows), 2)
                self.assertEqual(max(row.get("mirrorCount", 0) for row in rows), 2)
                ids = ",".join(row["id"] for row in rows)
                deadline = time.monotonic() + 8
                while service.command("metadata", ids=ids)["pending"] and time.monotonic() < deadline:
                    time.sleep(.02)
                rows = service.command("view", sourcePrefix="rs-dlps-")["remote"]
                self.assertEqual([row["provider"] for row in rows], ["Game - AkiraBox"])
                self.assertNotIn("mirrorCount", rows[0])
                states = service.command("replay", source="dlps", entry=server.feed["packages"][0])["links"]
                self.assertEqual([row["visible"] for row in states], [False, True, False])
                self.assertCountEqual(server.paths, ["/catalogue.json", viking, akira, singles])
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_metadata_checks_all_mirrors_and_repeated_urls_across_catalogues(self):
        server = FixtureServer()
        base = server.url.removesuffix("/catalogue.json")
        first = "/metadata/akirabox.com/game"
        second = "/metadata/vikingfile.com/game"
        third = "/metadata/buzzheavier.com/game"
        links = [{"url": base + path, "name": label} for path, label in
                 ((first, "Game - AkiraBox"), (second, "Game - VikingFile"), (third, "Game - BuzzHeavier"))]
        game = {"title": "Cyberpunk 2077", "titleId": "PPSA12345"}
        server.feed = {"packages": [dict(game, downloadLinks=links)]}
        server.feeds["/second.json"] = {"packages": [dict(game, downloadLinks=links[:1])]}
        html = {"Content-Type": "text/html"}
        server.downloads.update({
            first: (403, {**html, "cf-mitigated": "challenge"}, b"Verify"),
            second: (200, html, b"<p>File size: 1 GB</p>"),
            third: (200, html, b'<p data-size-bytes="930000000">0.93 GB</p>'),
        })
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory, Driver(directory, server.url, base + "/second.json") as service:
                self.assertEqual(service.command("start"), 0)
                service.idle()
                rows = [row for row in service.command("view")["remote"]
                        if row["id"].startswith(("rs-dlps-", "rs-pfs-"))]
                self.assertEqual(len(rows), 1)
                self.assertEqual(rows[0]["mirrorCount"], 3)
                ids = ",".join(row["id"] for row in rows)
                deadline = time.monotonic() + 8
                while True:
                    result = service.command("metadata", ids=ids)
                    if not result["pending"] or time.monotonic() >= deadline:
                        break
                    time.sleep(.02)
                self.assertFalse(result["pending"])
                self.assertEqual(len(result["options"]), 3)
                self.assertEqual([row["status"] for row in result["options"]].count("challenge"), 1)
                group, = result["files"]
                member = next(row for row in result["options"] if row["label"] == group["provider"] and row["visible"])
                self.assertEqual((group["sizeBytes"], group["exact"]), (member["sizeBytes"], member["exact"]))
                self.assertCountEqual(server.paths, ["/catalogue.json", "/second.json", first, second, third])
                duplicate, = service.command("replay", source="pfs", entry=server.feeds["/second.json"]["packages"][0])["links"]
                self.assertFalse(duplicate["visible"])
                self.assertTrue(duplicate["mirrorGroup"])
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_same_url_uses_one_get_and_keeps_distinct_format_hints(self):
        server = FixtureServer()
        base = server.url.removesuffix("/catalogue.json")
        path = "/metadata/shared"
        server.downloads[path] = (200, {"Content-Type": "text/html"},
                                  b'<h1>Shared.pkg</h1><p class="size">12 GB</p>')
        server.feed = {"packages": [{"title": "Format fixture", "titleId": "CUSA98765",
                       "downloadLinks": [{"url": base + path, "name": label, "format": label}
                                         for label in ("PKG", "FPKG")]}]}
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory, Driver(directory, server.url) as service:
                self.assertEqual(service.command("start"), 0)
                service.idle()
                rows = service.command("view", sourcePrefix="rs-dlps-")["remote"]
                self.assertCountEqual([row["format"] for row in rows], ["PKG", "FPKG"])
                ids = ",".join(row["id"] for row in rows)
                deadline = time.monotonic() + 5
                while True:
                    result = service.command("metadata", ids=ids)
                    if not result["pending"] or time.monotonic() >= deadline:
                        break
                    time.sleep(.02)
                self.assertCountEqual([row["format"] for row in result["files"]], ["PKG", "FPKG"])
                self.assertEqual(server.paths.count(path), 1)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_exact_provider_files_become_mirrors_across_catalogues(self):
        server = FixtureServer()
        base = server.url.removesuffix("/catalogue.json")
        game = {"title": "File identity test", "titleId": "PPSA12345"}

        def option(label, filename="game.pkg", size='data-size-bytes="930000000"', **fields):
            path = f"/metadata/{label}"
            server.downloads[path] = (
                200, {"Content-Type": "text/html"}, f"<h1>{filename}</h1><p {size}>0.93 GB</p>".encode()
            )
            return {"name": label, "url": base + path, **fields}

        server.feed = {"packages": [dict(game, downloadLinks=[option("alpha"), option("estimate", size="")])]}
        server.feeds["/second.json"] = {"packages": [dict(game, downloadLinks=[
            option("beta"), option("different-file", filename="other.pkg"), option("different-version", version="2.0"),
            option("different-size", size='data-size-bytes="930000001"'),
        ])]}
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory, Driver(directory, server.url, base + "/second.json") as service:
                self.assertEqual(service.command("start"), 0)
                service.idle()
                before = [row for row in service.command("view")["remote"] if row["titleId"] == game["titleId"]]
                self.assertEqual(len(before), 6)
                ids = ",".join(row["id"] for row in before)
                deadline = time.monotonic() + 8
                while True:
                    result = service.command("metadata", ids=ids)
                    if not result["pending"] or time.monotonic() >= deadline:
                        break
                    time.sleep(.02)
                self.assertFalse(result["pending"])
                after = [row for row in service.command("view")["remote"] if row["titleId"] == game["titleId"]]
                mirror, = [row for row in after if row.get("mirrorCount")]
                self.assertEqual(mirror["mirrorCount"], 2)
                self.assertIn(mirror["provider"], {"alpha", "beta"})
                self.assertCountEqual([row["provider"] for row in after if row != mirror],
                                      ["estimate", "different-file", "different-version", "different-size"])
                metadata = service.command("metadata", ids=mirror["id"])
                self.assertCountEqual([row["label"] for row in metadata["options"]], ["alpha", "beta"])
                self.assertCountEqual([path for path in server.paths if path.startswith("/metadata/")], server.downloads)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_tick_retries_only_empty_catalogues_after_cooldown(self):
        server = FixtureServer()
        server.send_etag = False
        server.feed = {"packages": []}
        server.feeds["/populated.json"] = {"packages": FIXTURE["packages"][:1]}
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            populated = server.url.replace("catalogue", "populated")
            with tempfile.TemporaryDirectory() as directory, Driver(directory, server.url, populated) as service:
                self.assertEqual(service.command("start"), 0)
                snapshot = service.idle()
                self.assertCountEqual(server.paths, ["/catalogue.json", "/populated.json"])
                self.assertEqual(source(snapshot, "dlps")["entryCount"], 0)
                self.assertEqual(source(snapshot, "pfs")["entryCount"], 1)
                service.command("advance", milliseconds=59999, tick=True)
                self.assertEqual(len(server.paths), 2)
                with server.lock:
                    server.feed = {"packages": FIXTURE["packages"][:1]}
                service.command("advance", milliseconds=1, tick=True)
                self.assertEqual(source(service.idle(), "dlps")["entryCount"], 1)
                self.assertEqual(server.paths[2:], ["/catalogue.json"])
                service.command("advance", milliseconds=60000, tick=True)
                self.assertEqual(len(server.paths), 3)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_conditional_refresh_offline_cache_and_builtin_bytes(self):
        server = FixtureServer()
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory:
                with Driver(directory, server.url) as service:
                    self._exercise_refresh(service, server)
                state_path = Path(directory) / "state.json"
                state = json.loads(state_path.read_text())
                state["jobs"][0]["error"] = "Source disabled. Enable it in Sources before resuming."
                state_path.write_text(json.dumps(state))
                with Driver(directory, server.url) as service:
                    view = service.command("view", sourcePrefix="rs-dlps-")
                    self.assertEqual(len(view["remote"]), 2)
                    self.assertEqual(len(server.requests), 4)  # Startup cache never needs a request.
                    jobs = service.command("jobs")
                    self.assertEqual(jobs[0]["status"], "paused")
                    self.assertFalse(jobs[0].get("error"))
                    self.assertEqual(jobs[0]["detectedFormat"], "Game folder")
                    self.assertEqual(jobs[0]["extractTotal"], 41)
                    snapshot = service.command("show")
                    cached = source(snapshot, "dlps")
                    self.assertEqual(cached["etag"], '"fixture-v1"')
                    self.assertEqual(cached["entryCount"], 2)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def _exercise_refresh(self, service, server):
        good, unsupported = copy.deepcopy(FIXTURE["packages"])
        unsupported["format"] = "ZSTD"
        attached = dict(good, titleId=service.first_game["titleId"], filename="game.zip",
                        category="Game folder", sizeBytes=37, extractedBytes=41,
                        downloadLinks=[{"url": "https://example.org/game.zip"}])
        invalid = dict(good, filename="../bad.pkg")
        server.feed = {"packages": [good, unsupported, attached, invalid]}
        source_id = "dlps"
        self.assertEqual(service.command("start"), 0)
        refreshed = source(service.idle(), source_id)
        self.assertEqual(server.requests, [None])
        self.assertEqual(refreshed["etag"], '"fixture-v1"')
        self.assertGreater(refreshed["lastFetchedAt"], 0)
        self.assertEqual(refreshed["entryCount"], 2)
        self.assertEqual(refreshed["skippedEntries"], 1)

        view = service.command("view", sourcePrefix="rs-dlps-")
        self.assertFalse(view["sameGames"])
        self.assertTrue(view["sameAddons"])
        by_title_id = {row["titleId"]: row for row in view["remote"]}
        self.assertTrue(by_title_id[good["titleId"]]["homebrew"])
        matching = by_title_id[attached["titleId"]]
        self.assertNotIn("homebrew", matching)
        self.assertEqual(matching["gameId"], service.first_game["gameId"])
        self.assertEqual(matching["compression"], "zip")
        self.assertNotIn("url", matching)
        service.command("save-job", id=matching["id"])

        service.command("refresh")
        cached = source(service.idle(), source_id)
        self.assertEqual(server.requests, [None, '"fixture-v1"'])
        self.assertEqual(cached["entryCount"], 2)
        self.assertEqual(service.command("view", sourcePrefix="rs-dlps-"), view)
        for response_code, error in ((503, "http"), (200, "invalid-catalogue")):
            with server.lock:
                server.failure = response_code
            service.command("refresh")
            failed = source(service.idle(), source_id)
            self.assertEqual(failed["lastError"], error)
            self.assertEqual(failed["entryCount"], 2)
        self.assertEqual(service.command("view", sourcePrefix="rs-dlps-"), view)
        diagnostic = service.command("diagnostics")
        self.assertIn("invalid-catalogue", diagnostic["lastErrorCodes"])
        self.assertNotIn(server.url, json.dumps(diagnostic))
