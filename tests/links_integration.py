"""Exercise the 1.9.7 link queue and ShadowMountPlus handoff over loopback."""

import io
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen
from zipfile import ZipFile

from torbox_routing_integration import BINARY, FILE, ROOT, wait_for


class Fixture(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, data, status=200, headers=()):
        self.send_response(status)
        self.send_header("Content-Length", str(len(data)))
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    def do_GET(self):
        path = urlsplit(self.path).path
        if path == "/catalogue.json":
            entry = {"title": "UFS provider fixture", "titleId": "PPSA99998", "downloadLinks": [{
                "name": "UFS2", "url": f"http://127.0.0.1:{self.server.server_port}/remote-image",
            }]}
            self.reply(json.dumps({"packages": [entry]}).encode(), headers=[("Content-Type", "application/json")])
            return
        if path not in ("/game.pkg", "/disc.ffpkg", "/image.zip", "/remote-image", "/part.bin", "/replacement.bin",
                        "/orbit-package/" + "f" * 64 + ".pkg"):
            self.reply(b"", 404)
            return
        if path == "/part.bin" and "Range" not in self.headers:
            self.reply(b"", 410)
            return
        data = {"/image.zip": self.server.image_archive, "/remote-image": self.server.ufs_image}.get(path, FILE)
        start, end, status = 0, len(data) - 1, 200
        headers = [("Content-Type", "application/octet-stream"), ("ETag", '"file"')]
        if path == "/remote-image":
            headers.append(("Content-Disposition", 'attachment; filename="remote-disc"'))
        if self.headers.get("Range"):
            first, last = self.headers["Range"].removeprefix("bytes=").split("-")
            start, end, status = int(first), min(int(last), end) if last else end, 206
            headers.append(("Content-Range", f"bytes {start}-{end}/{len(data)}"))
        if path == "/part.bin" and end > start:
            self.reply(b"", 410)
            return
        self.reply(data[start:end + 1], status, headers)

    do_HEAD = do_GET

    def do_POST(self):
        path = urlsplit(self.path).path
        raw = self.rfile.read(int(self.headers["Content-Length"]))
        body = {} if path.endswith("/chunk") else json.loads(raw)
        result, status = {"status": 0}, 200
        if path == "/api/v1/version":
            result.update(api_version=1, shadowmount_version="1.7",
                          capabilities=["list_games", "rescan", "add_manual_source",
                                        "pkg_install", "pkg_browser_stream"])
        elif path == "/api/v1/games":
            result.update(games=self.server.games, count=len(self.server.games))
        elif path == "/api/v1/scan":
            image = self.server.homebrew / "disc.ffpkg"
            if image.exists():
                self.server.games = [dict(title_id="PPSA99999", title_name="Test game",
                                          path=str(image), runtime_path="", image_type="ufs",
                                          platform="ps5", source_type="image", installed=True,
                                          mounted=False, source_available=True)]
        elif path == "/api/v1/packages/install":
            self.server.installs.append(body["path"])
            result["job_id"], status = 12, 202
        elif path == "/api/v1/packages/stream":
            self.server.stream = body
            result.update(job_id=13, token="b" * 32)
            status = 202
        elif path == "/api/v1/packages/stream/request":
            complete = self.server.stream_bytes == FILE
            result.update(id=0 if complete else 14, offset=0, size=len(FILE))
        elif path == "/api/v1/packages/stream/chunk":
            self.server.stream_bytes = raw
        elif path == "/api/v1/games/storage/status":
            job = body["job_id"]
            stream = job == 13
            complete = not stream or self.server.stream_bytes == FILE
            result.update(job_id=job, operation="install", state="completed" if complete else "installing",
                          source_type="stream" if stream else "pkg", total_bytes=len(FILE),
                          processed_bytes=len(FILE) if complete else 0,
                          source=self.server.stream["name"] if stream else self.server.installs[-1])
        else:
            result, status = {"status": 1, "error": "fixture route missing"}, 404
        self.reply(json.dumps(result).encode(), status, [("Content-Type", "application/json")])


class LinkDownloads(unittest.TestCase):
    def test_saved_links_and_shadowmount_installation(self):
        with tempfile.TemporaryDirectory() as directory, ThreadingHTTPServer(("127.0.0.1", 0), Fixture) as server:
            root = Path(directory)
            server.homebrew = root / "drive" / "homebrew"
            server.homebrew.mkdir(parents=True)
            server.games, server.installs, server.stream, server.stream_bytes = [], [], None, b""
            pfs = (2).to_bytes(8, "little") + (20130315).to_bytes(8, "little")
            archive = io.BytesIO()
            with ZipFile(archive, "w") as zip_file:
                zip_file.writestr("game.ffpfs", pfs)
            server.image_archive = archive.getvalue()
            server.ufs_image = bytes(65536 + 1372) + (0x19540119).to_bytes(4, "little") + b"data"
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            host = f"http://127.0.0.1:{server.server_port}"
            env = dict(os.environ, ORBIT_TEST_LIBRARY_PORT=str(server.server_port),
                       ORBIT_TEST_URL=host + "/game.pkg", ORBIT_TEST_SIZE=str(len(FILE)),
                       ORBIT_TEST_CATALOGUE_URL=host + "/catalogue.json")
            token = ""
            service = None

            def call(path, body=None):
                headers = {"Content-Type": "application/json", "Authorization": f"Bearer {token}"}
                data = None if body is None else json.dumps(body).encode()
                req = Request(f"http://127.0.0.1:{port}/api/v1{path}", data, headers)
                with urlopen(req, timeout=3) as response:
                    return json.load(response)

            def connect():
                try:
                    return call("/session")["token"]
                except URLError:
                    return None

            def job_status(job_id, state):
                return next((j for j in call("/downloads") if j["id"] == job_id and j["status"] == state), None)

            def record(origin, state):
                return next((r for r in call("/installations")["downloads"] if r["origin"] == origin
                             and any(p["state"] == state for p in r["packages"])), None)

            with (root / "orbit.log").open("w+") as log:
                def start():
                    return subprocess.Popen(
                        [BINARY, "--port", str(port), "--storage", str(server.homebrew.parent),
                         "--state", str(root / "state")], cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                    )

                try:
                    service = start()
                    token = wait_for(connect)
                    with self.assertRaises(HTTPError) as old:
                        call("/downloads/direct", {"url": host + "/game.pkg", "storageId": "desktop"})
                    self.assertEqual(old.exception.code, 404)
                    call("/links", {"urls": [host + url for url in ("/game.pkg", "/disc.ffpkg", "/part.bin", "/image.zip")],
                                    "storageId": "desktop"})

                    def checked():
                        batch = call("/links")
                        return batch["items"] if batch["state"] == "done" else None

                    items = wait_for(checked)
                    jobs = [item["jobId"] for item in items]
                    self.assertTrue(all(jobs), items)
                    self.assertEqual(wait_for(lambda: job_status(jobs[0], "complete"))["sourceId"], "link")
                    package = wait_for(lambda: record(jobs[0], "ready"))
                    self.assertTrue(package["available"])
                    call("/installations", {"action": "install", "downloadId": package["id"],
                                            "packageId": package["packages"][0]["id"]})
                    wait_for(lambda: record(jobs[0], "installed"))
                    self.assertEqual(server.installs, [str(server.homebrew / "game.pkg")])
                    space = call("/space", {"action": "list", "storageId": "desktop"})
                    self.assertEqual(space["storageId"], "desktop")
                    self.assertTrue(any(item["kind"] == "package" for item in space["leftovers"]))

                    def registered():
                        job = job_status(jobs[1], "complete")
                        if job and (job.get("installation") or {}).get("shadowMount") == "registered":
                            return job

                    self.assertEqual(wait_for(registered)["format"], "FFPKG")
                    self.assertEqual(wait_for(lambda: job_status(jobs[3], "complete"))["detectedFormat"], "FFPFS")
                    self.assertEqual((server.homebrew / "game.ffpfs").read_bytes(), pfs)
                    wait_for(lambda: job_status(jobs[2], "error"))
                    call(f"/downloads/{jobs[2]}/replace-link", {"url": host + "/replacement.bin"})
                    wait_for(checked)
                    job = wait_for(lambda: job_status(jobs[2], "complete"))
                    self.assertEqual((server.homebrew / job.get("output", job["filename"])).read_bytes(), FILE)

                    release = wait_for(lambda: next((r for r in call("/catalog") if r["title"] == "UFS provider fixture"), None))
                    remote = call("/downloads", {"releaseId": release["id"], "storageId": "desktop", "delivery": "direct"})
                    self.assertEqual(wait_for(lambda: job_status(remote["id"], "complete"))["format"], "FFPKG")
                    self.assertEqual((server.homebrew / "remote-disc.ffpkg").read_bytes(), server.ufs_image)

                    stream_id = "e" * 32
                    call("/installations/stream", {"streamId": stream_id, "token": "f" * 64,
                                                  "filename": "stream.fpkg", "port": server.server_port,
                                                  "sizeBytes": len(FILE)})
                    wait_for(lambda: record(stream_id, "installed"))
                    self.assertEqual(server.stream_bytes, FILE)
                    self.assertEqual(server.stream["name"], "stream.pkg")

                    service.terminate()
                    service.wait(timeout=15)
                    queue = root / "state" / "state.json"
                    saved = json.loads(queue.read_text())
                    legacy = next(item for item in saved["jobs"] if item["id"] == jobs[1])
                    legacy["release"]["format"] = "File"
                    queue.write_text(json.dumps(saved))
                    service = start()
                    token = wait_for(connect)
                    self.assertEqual(wait_for(registered)["format"], "FFPKG")
                    self.assertEqual(job_status(jobs[3], "complete")["detectedFormat"], "FFPFS")
                    self.assertEqual(job_status(remote["id"], "complete")["filename"], "remote-disc.ffpkg")
                    self.assertIsNotNone(wait_for(lambda: job_status(jobs[2], "complete")))
                    self.assertIsNotNone(record(jobs[0], "installed"))
                except Exception:
                    log.seek(0)
                    print(log.read())
                    raise
                finally:
                    if service and service.poll() is None:
                        service.terminate()
                        service.wait(timeout=15)
                    server.shutdown()
                    thread.join()


if __name__ == "__main__":
    unittest.main()
