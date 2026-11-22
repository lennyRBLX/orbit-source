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
DEFAULTS = {"dlps", "evox-coreos", "pfs", "pippo-exfat", "zer0"}


class SourceHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, _format, *args):
        pass

    def do_GET(self):
        with self.server.lock:
            validator = self.headers.get("If-None-Match")
            self.server.requests.append(validator)
            self.server.paths.append(self.path)
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
                self.assertEqual(len({row["gameId"] for row in rows}), 1)
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
            settings["sources"] = [s for s in settings["sources"] if s["id"] != "zer0"]
            path.write_text(json.dumps(settings))
            with Driver(directory) as service:
                snapshot = service.command("show")
                self.assertEqual(snapshot["revision"], settings["revision"] + 1)
                self.assertFalse(source(snapshot, "evox-coreos")["enabled"])
                self.assertTrue(source(snapshot, "zer0")["enabled"])

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
