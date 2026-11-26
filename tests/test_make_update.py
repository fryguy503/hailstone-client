import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("make_update", ROOT / "tools/make_update.py")
make_update = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_update)


class PlayerPathTests(unittest.TestCase):
    # The same cases as tests/updater_test.cpp "the player's files are never an update's": the packager's copy of
    # is_player_path must keep out exactly what the client refuses to install.
    def test_player_paths_match_the_client(self):
        for p in ("hailstone.ini", "HAILSTONE.INI", "servers/a/server.ini", "logs/hailstone.log", "update/state.txt",
                  "ui/settings/modern.txt", "ui/loadingbackgrounds/a.png", "maps/x.txt", "scripts/a.lua",
                  "textures/default/a.dds", "web/a/b", "notes.txt", "Servers/x", "tools/texstudio/texstudio",
                  "Tools/x/data.txt", "ui-data/modern/lib/style.lua",
                  "UI-DATA/addon.bundle.skin/lib/compiled/window.lua", "ui-data/", "ui-data", "UI-DATA",
                  "ui-addons/skin/ui/dark/ui.ini", "UI-ADDONS/skin/ui/dark/window.lua", "ui-addons",
                  "upscalers/example/example.so", "Upscalers/x/notice.txt"):
            self.assertTrue(make_update.is_player_path(p), p)
        for p in ("hailstone", "hailstone.exe", "shaders/a.spv", "ui/modern/login.lua", "browser/libcef.so",
                  "plugins/nms/nms.so", "icons/hailstone.png", "servers.txt", "ui-data-copy/file.lua",
                  "ui-database/file.lua", "ui-data.lua", "ui-addons.lua", "upscalers.txt"):
            self.assertFalse(make_update.is_player_path(p), p)


def run(*args):
    return subprocess.run([sys.executable, str(ROOT / "tools/make_update.py"), *map(str, args)], capture_output=True,
                          text=True)


class ReleaseKeyTests(unittest.TestCase):
    # A binary built with another HAILSTONE_UPDATE_KEY (a reused build folder's test key, or none) would never take the
    # next release's update: make_update.py refuses to package it.
    KEY = "ab" * 32

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.dist = self.root / "dist"
        self.dist.mkdir()
        self.pub = self.root / "update-key.pub"
        self.pub.write_text(self.KEY + "\n")

    def make(self, binary_text, name="hailstone", key=None):
        (self.dist / name).write_bytes(b"\x7fELF..." + binary_text.encode() + b"\0...")
        return run("--version", "1.0.0", "--platform", "linux-x64", "--dist", self.dist, "--key", key or self.pub,
                   "--out", self.root / "out")

    def test_a_binary_with_the_key_is_packaged(self):
        result = self.make(self.KEY)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.root / "out" / "hailstone-1.0.0-linux-x64.zip").exists())

    def test_the_key_is_found_in_either_case(self):
        result = self.make(self.KEY.upper(), "hailstone.exe")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_a_binary_with_another_key_or_none_is_refused(self):
        for text in ("cd" * 32, "", self.KEY[:-1]):
            with self.subTest(text=text):
                result = self.make(text)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("does not hold the release key", result.stderr)
                self.assertFalse((self.root / "out" / "hailstone-1.0.0-linux-x64.zip").exists())

    def test_a_key_file_without_a_key_is_refused(self):
        bad = self.root / "bad.pub"
        bad.write_text("not a key\n")
        result = self.make(self.KEY, key=bad)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no public key", result.stderr)
        result = self.make(self.KEY, key=self.root / "missing.pub")
        self.assertNotEqual(result.returncode, 0)

    def test_the_default_key_is_the_trees_update_key_pub(self):
        key = make_update.read_key(ROOT / "update-key.pub")
        self.assertEqual(len(key), 64)
        (self.dist / "hailstone").write_text("x" + key + "y")
        result = run("--version", "1.0.0", "--platform", "linux-x64", "--dist", self.dist, "--out", self.root / "out")
        self.assertEqual(result.returncode, 0, result.stderr)


@unittest.skipUnless(shutil.which("bash") and os.name != "nt", "needs bash")
class ReleaseOptionsTests(unittest.TestCase):
    # tools/docker/release_options.sh: every release configure names the key from update-key.pub and resets what an
    # earlier configure of the same build folder may have left in its cache.
    def options(self, folder):
        script = ROOT / "tools/docker/release_options.sh"
        return subprocess.run(["bash", "-c", f'. "$0"; release_options "$1"; printf "%s\\n" "${{release[@]}}"',
                               str(script), str(folder)], capture_output=True, text=True)

    def test_the_key_comes_from_update_key_pub_and_the_cache_is_reset(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "update-key.pub").write_text("12" * 32 + "\n")
            result = self.options(tmp)
            self.assertEqual(result.returncode, 0, result.stderr)
            args = result.stdout.split("\n")
            for want in ("-DHAILSTONE_UPDATE_KEY=" + "12" * 32, "-DCMAKE_BUILD_TYPE=Release", "-DHAILSTONE_SANITIZE=OFF",
                         "-DHAILSTONE_LIBFUZZER=OFF", "-DHAILSTONE_ALLOW_IN_EQ_FOLDER=OFF", "-UHAILSTONE_UPDATE_FEED",
                         "-UHAILSTONE_ADDON_INDEX", "-UHAILSTONE_PROJECT_PAGE"):
                self.assertIn(want, args)

    def test_no_key_stops_the_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.options(tmp)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("no release key", result.stderr)
            Path(tmp, "update-key.pub").write_text("short\n")
            self.assertNotEqual(self.options(tmp).returncode, 0)


if __name__ == "__main__":
    unittest.main()
