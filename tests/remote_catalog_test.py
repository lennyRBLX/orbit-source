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
        self.failure = None
        self.feed = None
        self.feeds = {}
        self.downloads = {}
        self.send_etag = True
        self.url = f"http://127.0.0.1:{self.server_port}/catalogue.json"


class Driver:
    def __init__(self, directory):
        self.process = subprocess.Popen(
            [DRIVER, directory], cwd=ROOT, text=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE
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
            self.assertTrue(state["enabled"])
            self.assertTrue(state["bundled"])
            self.assertGreater(state["entryCount"], 10000)
            service.command("policy", remote=True, notice=False)
            self.assertFalse(service.command("view")["remote"])
            service.command("policy", remote=True, notice=True)
            view = service.command("view")
            rows = view["remote"]
            self.assertEqual(len(rows), state["entryCount"])
            self.assertGreater(len({row["titleId"] for row in rows}), 3500)
            self.assertEqual({row["platform"] for row in rows}, {"ps4", "ps5"})
            self.assertTrue(view["sameAddons"])
            self.assertTrue(any(row["gameId"] == service.first_game["gameId"] for row in rows))
            self.assertTrue(any(row["gameId"].startswith("rs-padrino-") and row["cover"] for row in rows))
            self.assertFalse(any(row.get("homebrew") or row.get("archivePassword") or
                                 "zstd" in row["filename"].lower() for row in rows))
            self.assertEqual(service.command("PATCH", id="padrino", enabled=False)["status"], 200)
            self.assertTrue(service.command("view")["sameGames"])

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
            self.assertEqual(service.command("map", entry=unsupported), 1)

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
            self.assertEqual(service.command("map", entry=dict(valid, filename="game.7z")), 1)
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
            with tempfile.TemporaryDirectory() as directory, Driver(directory) as service:
                base = {"title": "Game", "titleId": "PPSA12345", "category": "game"}

                def entry(path, label):
                    return dict(base, downloadLinks=[{"name": label, "url": address + path}])

                for label in ("Folder · LZ4", "FpKg / ZsTd", "FPKG · PFS", "exFAT"):
                    self.assertEqual(service.command("map", entry=entry("/game.zip", label)), 1)
                for suffix in (".7z", ".rar.001", ".part2.rar", ".exfat"):
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

                for name in DEFAULTS:
                    service.command("PATCH", id=name, enabled=False)
                options = [
                    {"name": "LZ4 · Viki", "url": address + "/game.zip"},
                    {"name": "Folder · Viki", "url": address + "/folder"},
                    {"name": "FPKG · Data", "url": address + "/mirror"},
                ]
                server.feed = {"packages": [dict(base, downloadLinks=options)]}
                source_setup = service.command("POST", url=server.url)
                self.assertEqual(source_setup["status"], 201)
                self.assertEqual(service.command("start"), 0)
                service.command("policy", remote=True, notice=True)
                service.idle()
                rows = service.command("view")["remote"]
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
            with tempfile.TemporaryDirectory() as directory, Driver(directory) as service:
                for name in DEFAULTS:
                    service.command("PATCH", id=name, enabled=False)
                service.command("POST", url=server.url)
                self.assertEqual(service.command("start"), 0)
                service.command("policy", remote=True, notice=True)
                service.idle()

                rows = service.command("view")["remote"]
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
            with tempfile.TemporaryDirectory() as directory, Driver(directory) as service:
                for name in DEFAULTS:
                    service.command("PATCH", id=name, enabled=False)
                service.command("POST", url=server.url)
                self.assertEqual(service.command("start"), 0)
                service.command("policy", remote=True, notice=True)
                service.idle()
                rows = service.command("view")["remote"]
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
                    [(row["sizeBytes"], row["exact"]) for row in result["files"]],
                    [(1480000000, False), (int(42.25 * 1024 ** 3), False), (43123456789, True)],
                )
                self.assertEqual(service.command("metadata", ids=ids), result)
                self.assertEqual(sum(path.startswith("/metadata/") for path in server.paths), 4)
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
            with tempfile.TemporaryDirectory() as directory, Driver(directory) as service:
                for name in DEFAULTS:
                    service.command("PATCH", id=name, enabled=False)
                service.command("POST", url=server.url)
                self.assertEqual(service.command("start"), 0)
                service.command("policy", remote=True, notice=True)
                service.idle()
                rows = service.command("view")["remote"]
                by_title = {}
                for row in rows:
                    self.assertNotIn("mirrorKey", row)
                    by_title.setdefault(row["titleId"], []).append(row)
                self.assertEqual([len(by_title[f"PPSA{i:05d}"]) for i in range(1, 8)], [2, 3, 2, 2, 2, 2, 2])
                base = next(row for row in by_title["PPSA00001"] if row.get("mirrorCount"))
                self.assertEqual(base["mirrorCount"], 2)
                self.assertCountEqual(base["torboxHosts"], ["akirabox.to", "vikingfile.com"])
                self.assertTrue(all(by_title[f"PPSA{i:05d}"][0]["mirrorCount"] == 2 for i in range(10000, 11025)))
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_community_catalogues_default_on_and_saved_opt_out_survives_restart(self):
        with tempfile.TemporaryDirectory() as directory:
            with Driver(directory) as service:
                fresh = service.command("selection")
                self.assertFalse(fresh["acknowledged"])
                self.assertEqual(fresh["enabled"], ["remote"])
                self.assertEqual(service.command("POST", id="refresh")["status"], 409)
                self.assertEqual(service.command("save-selection", enabled=["archive"],
                                                 noticeVersion=1, acknowledged=True), 200)
            with Driver(directory) as service:
                saved = service.command("selection")
                self.assertTrue(saved["acknowledged"])
                self.assertEqual(saved["enabled"], ["archive"])

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
                self.assertTrue(built_in["builtIn"])
                self.assertTrue(built_in["enabled"])
                self.assertEqual(built_in["url"], f"https://pspkg.com/{platform}/")
                parsed = service.command("parse", source=source_id, feed=feed)
                self.assertEqual((parsed["code"], parsed["skipped"]), (0, 0))
                self.assertEqual(len(parsed["releases"]), 1)
                self.assertEqual(parsed["releases"][0]["titleId"], expected)
                self.assertEqual(parsed["releases"][0]["platform"], platform)
            self.assertEqual(service.command("parse", source="pspkg-ps5",
                                             feed={"packages": [arabic]})["releases"], [])

    def test_pspkg_public_game_links_exclude_update_and_backport_sections(self):
        html = ("<div>Game PKG <a href=\"https://vikingfile.com/f/game123\">Download</a></div>"
                "<div>Update PKG <a href=\"https://vikingfile.com/f/update123\">Download</a></div>"
                "<div>Backport PKG <a href=\"https://example.org/backport.pkg\">Download</a></div>")
        with tempfile.TemporaryDirectory() as directory, Driver(directory) as service:
            links = service.command("pspkg-links", html=html)
            self.assertEqual([link["url"] for link in links], ["https://vikingfile.com/f/game123"])

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

    def test_pspkg_scan_fetches_general_ps5_and_ps4_pages(self):
        server = FixtureServer()
        for platform in ("ps5", "ps4"):
            server.downloads[f"/{platform}/?page=1"] = (200, {"Content-Type": "text/html"},
                (f'<a href="/{platform}/sample-game-123.html">Sample</a>'
                 '<a href="/arabic-games/arabic-game-456.html">Arabic</a>').encode())
            public = (b'<div>Game PKG <a href="https://vikingfile.com/f/game123">Download</a></div>'
                      if platform == "ps5" else b'<p>Downloads require verification</p>')
            server.downloads[f"/{platform}/sample-game-123.html"] = (200, {"Content-Type": "text/html"},
                b'<h1>Sample &amp; Game</h1>' + public)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory, Driver(directory) as service:
                service.command("policy", remote=True, notice=True)
                for platform in ("ps5", "ps4"):
                    result = service.command("pspkg-scan", source=f"pspkg-{platform}",
                                             url=server.url.replace("/catalogue.json", f"/{platform}/"),
                                             listingLimit=1, gameLimit=3)
                    self.assertEqual((result["error"], result["discovered"]), ("", 1))
                    packages = result["feed"]["packages"]
                    self.assertEqual(len(packages), 1)
                    self.assertEqual(packages[0]["title"], "Sample & Game")
                    self.assertEqual(packages[0]["category"], platform.upper())
                    links = packages[0]["downloadLinks"]
                    self.assertEqual(len(links), 2 if platform == "ps5" else 1)
                    self.assertEqual(links[-1], {
                        "name": "PSPKG",
                        "url": f"{server.url.replace('/catalogue.json', f'/{platform}/')}download-sample-game-123.html?id=123",
                        "page": True,
                    })
                    if platform == "ps5":
                        self.assertEqual(links[0]["url"], "https://vikingfile.com/f/game123")
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_manifest_revision_keeps_disabled_defaults(self):
        with tempfile.TemporaryDirectory() as directory:
            with Driver(directory) as service:
                self.assertEqual({s["id"] for s in service.command("show")["sources"]}, DEFAULTS)
                response = service.command("PATCH", id="evox-coreos", enabled=False)
                self.assertEqual(response["status"], 200)
                self.assertFalse(source(response, "evox-coreos")["enabled"])
                self.assertEqual(service.command("DELETE", id="evox-coreos")["status"], 409)
            path = Path(directory) / "remote-sources-v1.json"
            settings = json.loads(path.read_text())
            settings["revision"] -= 1
            settings["sources"] = [s for s in settings["sources"]
                                   if s["id"] not in {"zer0", "pspkg-ps5", "pspkg-ps4"}]
            path.write_text(json.dumps(settings))
            with Driver(directory) as service:
                snapshot = service.command("show")
                self.assertEqual(snapshot["revision"], settings["revision"] + 1)
                self.assertFalse(source(snapshot, "evox-coreos")["enabled"])
                self.assertTrue(source(snapshot, "zer0")["enabled"])
                self.assertTrue(source(snapshot, "pspkg-ps5")["enabled"])
                self.assertTrue(source(snapshot, "pspkg-ps4")["enabled"])

    def test_tick_retries_enabled_empty_sources_and_cools_down_manual_refresh(self):
        server = FixtureServer()
        server.send_etag = False
        server.feed = {"packages": []}
        server.feeds["/populated.json"] = {"packages": FIXTURE["packages"][:1]}
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory, Driver(directory) as service:
                for name in DEFAULTS:
                    self.assertEqual(service.command("PATCH", id=name, enabled=False)["status"], 200)
                options = service.command("POST", url=server.url)
                empty = next(row["id"] for row in options["sources"] if not row["builtIn"])
                self.assertEqual(service.command("POST", url=server.url.replace("catalogue", "populated"))["status"], 201)
                self.assertEqual(service.command("start"), 0)
                service.command("advance", milliseconds=60000, tick=True)
                self.assertEqual(server.paths, [])
                service.command("policy", remote=False, notice=True)
                snapshot = service.idle()
                self.assertCountEqual(server.paths, ["/catalogue.json", "/populated.json"])
                self.assertEqual(source(snapshot, empty)["entryCount"], 0)
                self.assertFalse(service.command("view")["remote"])

                service.command("advance", milliseconds=59999, tick=True)
                self.assertEqual(len(server.paths), 2)
                with server.lock:
                    server.feed = {"packages": FIXTURE["packages"][:1]}
                service.command("advance", milliseconds=1, tick=True)
                self.assertEqual(source(service.idle(), empty)["entryCount"], 1)
                self.assertEqual(server.paths[2:], ["/catalogue.json"])
                service.command("advance", milliseconds=60000, tick=True)
                self.assertEqual(len(server.paths), 3)

                manual = service.command("POST", id="refresh")
                self.assertEqual((manual["status"], manual["retryAfter"]), (202, 60))
                self.assertEqual(service.command("POST", id="refresh")["status"], 429)
                service.idle()
                service.command("advance", milliseconds=59999)
                limited = service.command("POST", id="refresh")
                self.assertEqual((limited["status"], limited["retryAfter"]), (429, 1))
                service.command("advance", milliseconds=1)
                self.assertEqual(service.command("POST", id="refresh")["status"], 202)
                service.idle()
                self.assertCountEqual(server.paths[3:],
                                      ["/catalogue.json", "/populated.json"] * 2)
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
                with Driver(directory) as service:
                    self._exercise_refresh(service, server)
                with Driver(directory) as service:
                    service.command("policy", remote=True, notice=True)
                    view = service.command("view")
                    self.assertEqual(len(view["remote"]), 2)
                    self.assertEqual(len(server.requests), 6)  # Startup cache never needs a request.
                    jobs = service.command("jobs")
                    self.assertEqual(jobs[0]["detectedFormat"], "Game folder")
                    self.assertEqual(jobs[0]["extractTotal"], 41)
                    snapshot = service.command("show")
                    custom = next(s for s in snapshot["sources"] if not s["builtIn"])
                    self.assertEqual(custom["etag"], '"fixture-v1"')
                    self.assertEqual(custom["entryCount"], 2)
                    self.assertEqual(service.command("DELETE", id=custom["id"])["status"], 200)
                    self.assertEqual(service.command("view"), {"sameGames": True, "sameAddons": True, "remote": []})
                    self.assertFalse((Path(directory) / "remote-cache" / f'{custom["id"]}.json').exists())
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def _exercise_refresh(self, service, server):
        for name in DEFAULTS:
            self.assertEqual(service.command("PATCH", id=name, enabled=False)["status"], 200)
        good, unsupported = copy.deepcopy(FIXTURE["packages"])
        attached = dict(good, titleId=service.first_game["titleId"], filename="game.zip",
                        category="Game folder", sizeBytes=37, extractedBytes=41,
                        downloadLinks=[{"url": "https://example.org/game.zip"}])
        invalid = dict(good, filename="../bad.pkg")
        server.feed = {"packages": [good, unsupported, attached, invalid]}
        response = service.command("POST", url=server.url, name="Local test catalogue")
        self.assertEqual(response["status"], 201)
        custom = next(s for s in response["sources"] if not s["builtIn"])
        source_id = custom["id"]
        self.assertEqual(service.command("POST", url=server.url + "?duplicate=1")["status"], 409)
        self.assertEqual(service.command("start"), 0)
        service.command("policy", remote=True, notice=False)
        self.assertEqual(service.command("POST", id="refresh")["status"], 409)
        self.assertEqual(server.requests, [])
        service.command("policy", remote=True, notice=True)
        refreshed = source(service.idle(), source_id)
        self.assertEqual(server.requests, [None])
        self.assertEqual(refreshed["etag"], '"fixture-v1"')
        self.assertGreater(refreshed["lastFetchedAt"], 0)
        self.assertEqual(refreshed["entryCount"], 2)
        self.assertEqual(refreshed["skippedEntries"], 1)

        view = service.command("view")
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

        self.assertEqual(service.command("POST", id="refresh")["status"], 202)
        cached = source(service.idle(), source_id)
        self.assertEqual(server.requests, [None, '"fixture-v1"'])
        self.assertEqual(cached["entryCount"], 2)
        self.assertEqual(service.command("view"), view)
        for response_code, error in ((503, "http"), (200, "invalid-catalogue")):
            service.command("advance", milliseconds=60000)
            with server.lock:
                server.failure = response_code
            self.assertEqual(service.command("POST", id="refresh")["status"], 202)
            failed = source(service.idle(), source_id)
            self.assertEqual(failed["lastError"], error)
            self.assertEqual(failed["entryCount"], 2)
        self.assertEqual(service.command("view"), view)
        diagnostic = service.command("diagnostics")
        self.assertIn("invalid-catalogue", diagnostic["lastErrorCodes"])
        self.assertNotIn(server.url, json.dumps(diagnostic))
        self.assertEqual(service.command("PATCH", id=source_id, enabled=False)["status"], 200)
        self.assertEqual(service.command("view"), {"sameGames": True, "sameAddons": True, "remote": []})
        service.command("policy", remote=False, notice=True)
        self.assertEqual(service.command("PATCH", id=source_id, enabled=True)["status"], 200)
        service.idle()
        self.assertEqual(len(server.requests), 5)
        self.assertEqual(service.command("view"), {"sameGames": True, "sameAddons": True, "remote": []})
        service.command("advance", milliseconds=60000)
        self.assertEqual(service.command("POST", id="refresh")["status"], 202)
        service.idle()
        self.assertEqual(len(server.requests), 6)
