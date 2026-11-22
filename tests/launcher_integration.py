import json
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / "build/orbit-launcher-test"


class LauncherTest(unittest.TestCase):
    def setUp(self):
        root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.state = root / "state"
        self.user_apps = root / "app"
        self.state.mkdir()
        self.user_apps.mkdir()
        self.title = self.user_apps / "ORBT00001" / "sce_sys"
        self.registration = self.state / "fixture-registration"

    def ensure(self, scenario="ok", **kwargs):
        output = subprocess.check_output([DRIVER, self.state, self.user_apps, scenario], text=True, **kwargs)
        summary = next(line for line in output.splitlines() if line.startswith("result "))
        return list(map(int, summary.split()[1:])), output

    def test_missing_system_entry_restored_without_rewriting_intact_artwork(self):
        self.assertEqual(self.ensure()[0], [1, 1, 1])
        manifest = self.title / "param.json"
        icon = self.title / "icon0.png"
        self.assertEqual(json.loads(manifest.read_text())["applicationCategoryType"], 65536)
        original = manifest.stat().st_mtime_ns, icon.stat().st_mtime_ns
        self.assertEqual(self.ensure()[0], [0, 0, 0])

        self.registration.unlink()
        self.assertEqual(self.ensure()[0], [1, 1, 1])
        self.assertTrue(self.registration.exists())
        self.assertEqual((manifest.stat().st_mtime_ns, icon.stat().st_mtime_ns), original)

    def test_failed_repair_retries(self):
        self.assertEqual(self.ensure()[0], [1, 1, 1])
        self.registration.unlink()
        result, log = self.ensure("failed")
        self.assertEqual(result, [-1, 1, 1])
        self.assertIn("register-title: errno=0, platform=0x00000007", log)
        result, log = self.ensure("unavailable")
        self.assertEqual(result, [-1, 1, 0])
        self.assertIn("title-registration-unavailable", log)
        self.assertEqual(self.ensure()[0], [1, 1, 1])

    def test_missing_icon_and_unrelated_title(self):
        other = self.user_apps / "ABCD12345"
        other.mkdir()
        sentinel = other / "icon0.png"
        sentinel.write_bytes(b"unrelated title")
        self.assertEqual(self.ensure()[0], [1, 1, 1])
        icon = self.title / "icon0.png"
        icon.unlink()
        self.assertEqual(self.ensure()[0], [1, 1, 1])
        self.assertGreater(icon.stat().st_size, 0)
        self.assertEqual(sentinel.read_bytes(), b"unrelated title")

    def test_system_app_service_can_traverse_title_under_private_umask(self):
        self.assertEqual(self.ensure(umask=0o077)[0], [1, 1, 1])
        for directory in (self.title, self.title.parent):
            self.assertEqual(directory.stat().st_mode & 0o055, 0o055)
        self.title.chmod(0o700)
        self.title.parent.chmod(0o700)
        self.registration.unlink()
        self.assertEqual(self.ensure(umask=0o077)[0], [1, 1, 1])
        for directory in (self.title, self.title.parent):
            self.assertEqual(directory.stat().st_mode & 0o055, 0o055)

    def test_two_launches_register_once(self):
        commands = [DRIVER, self.state, self.user_apps, "ok"]
        a = subprocess.Popen(commands, stdout=subprocess.PIPE, text=True)
        b = subprocess.Popen(commands, stdout=subprocess.PIPE, text=True)
        summaries = []
        for process in [a, b]:
            output, _ = process.communicate(timeout=10)
            self.assertEqual(process.returncode, 0, output)
            summaries.extend(line for line in output.splitlines() if line.startswith("result "))
        self.assertCountEqual(summaries, ["result 0 0 0", "result 1 1 1"])


if __name__ == "__main__":
    unittest.main()
