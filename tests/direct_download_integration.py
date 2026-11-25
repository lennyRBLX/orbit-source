"""Exercise Direct URL verification, queueing and extraction with real 7z files."""

import json
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import unittest
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

from torbox_routing_integration import BINARY, ROOT, wait_for


class Fixture(SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass


class DirectDownload(unittest.TestCase):
    def test_encrypted_and_plain_archives(self):
        secret = " test-pässword "
        payload = b"Direct URL archive contents\n"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            files = root / "files"
            files.mkdir()
            homebrew = root / "drive" / "homebrew"
            homebrew.mkdir(parents=True)
            cases = (("encrypted", secret), ("retry", "wrong"), ("plain", None))
            for name, password in cases:
                (files / f"{name}.bin").write_bytes(payload)
                flags = [f"-p{secret}", "-mhe=on"] if password else []
                subprocess.run(["7z", "a", *flags, f"{name}.7z", f"{name}.bin"],
                               cwd=files, check=True, stdout=subprocess.DEVNULL)

            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            with ThreadingHTTPServer(("127.0.0.1", 0), partial(Fixture, directory=files)) as fixture:
                thread = threading.Thread(target=fixture.serve_forever, daemon=True)
                thread.start()
                env = dict(os.environ, ORBIT_TEST_URL=f"http://127.0.0.1:{fixture.server_port}/plain.7z",
                           ORBIT_TEST_SIZE=str((files / "plain.7z").stat().st_size))
                with (root / "orbit.log").open("w+") as log:
                    service = subprocess.Popen(
                        [BINARY, "--port", str(port), "--storage", str(homebrew.parent),
                         "--state", str(root / "state")], cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                    )
                    try:
                        self.check_downloads(port, fixture.server_port, cases, secret, payload, homebrew)
                    except Exception:
                        log.seek(0)
                        print(log.read())
                        raise
                    finally:
                        service.terminate()
                        service.wait(timeout=15)
                        fixture.shutdown()
                        thread.join()

    def check_downloads(self, port, fixture_port, cases, secret, payload, homebrew):
        token = ""

        def call(path, body=None):
            headers = {"Content-Type": "application/json"}
            if token:
                headers["Authorization"] = "Bearer " + token
            data = None if body is None else json.dumps(body).encode()
            request = Request(f"http://127.0.0.1:{port}/api/v1{path}", data, headers)
            with urlopen(request, timeout=3) as reply:
                result = json.load(reply)
            self.assertNotIn(secret, json.dumps(result, ensure_ascii=False))
            return result

        def connect():
            try:
                return call("/session")["token"]
            except URLError:
                return None

        def queued():
            status = call("/browser")
            return status if status["state"] in ("queued", "failed") else None

        def finished():
            return next((job for job in call("/downloads") if job["id"] == job_id
                         and job["status"] in ("complete", "error")), None)

        token = wait_for(connect)
        for name, password in cases:
            with self.subTest(name=name):
                request = {"url": f"http://127.0.0.1:{fixture_port}/{name}.7z", "storageId": "desktop"}
                if password is not None:
                    request["password"] = password
                if name == "encrypted":
                    for invalid in (42, "é" * 65, "password\n"):
                        with self.assertRaises(HTTPError) as error:
                            call("/downloads/direct", {**request, "password": invalid})
                        self.assertEqual(error.exception.code, 400)

                call("/downloads/direct", request)
                status = wait_for(queued)
                self.assertEqual(status["state"], "queued", status)
                job_id = status["jobId"]
                job = wait_for(finished)
                if name == "retry":
                    self.assertEqual(job["status"], "error", job)
                    self.assertTrue(job["canEnterArchivePassword"])
                    call(f"/downloads/{job_id}/archive-password", {"password": secret})
                    job = wait_for(finished)
                self.assertEqual(job["status"], "complete", job)
                self.assertEqual(job["compression"], "7z")
                self.assertEqual((homebrew / job["output"]).read_bytes(), payload)


if __name__ == "__main__":
    unittest.main()
