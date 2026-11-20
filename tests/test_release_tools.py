"""tools/make_addon.py and tools/make_update.py: versions the client parses, names its zip reader takes, and what
goes into a package (no --out inside it, no link out of it, no dotfiles)."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


make_addon = load("make_addon")
make_update = load("make_update")


def run(tool, *args):
    return subprocess.run([sys.executable, str(ROOT / "tools" / tool), *map(str, args)], capture_output=True, text=True)


class VersionTests(unittest.TestCase):
    def test_leading_zeros_are_refused_as_core_version_refuses_them(self):
        for module in (make_addon, make_update):
            for good in ("0.0.0", "1.2.3", "10.20.30", "1.2.3-beta.1"):
                self.assertTrue(module.VERSION.match(good), good)
            for bad in ("01.2.3", "1.02.3", "1.2.03", "00.0.0", "1.2", "1.2.3.4", "1.2.3-beta.01", "1.2.3-",
                        "1.2.3-a..b"):
                self.assertFalse(module.VERSION.match(bad), bad)
        self.assertTrue(make_addon.VERSION.match("v1.2.3+build.05"))
        self.assertFalse(make_addon.VERSION.match("1.2.3+a..b"))


class ZipNameTests(unittest.TestCase):
    def test_names_follow_core_zip_read_is_safe_zip_path(self):
        for module in (make_addon, make_update):
            for good in ("init.lua", "ui/modern/a.lua", "com0.txt", "console.lua", "auxiliary.lua", "a.b.c", "nul_x"):
                self.assertTrue(module.is_safe_zip_path(good), good)
            for bad in ("", "/abs", "aux.lua", "AUX", "ui/nul.txt", "con .txt", "com1.dds", "LPT9", "com¹.x",
                        "conin$.y", "a.lua.", "a /b", "c:x", "a\\b", "x\x01", "x\x7f", "..", "a/./b", "a//b"):
                self.assertFalse(module.is_safe_zip_path(bad), repr(bad))

    def test_make_addon_check_refuses_a_name_the_client_would(self):
        for files in (["init.lua", "aux.lua"], ["init.lua", "data/notes.txt."], ["init.lua", "x:y.lua"]):
            with self.subTest(files=files), self.assertRaises(SystemExit):
                make_addon.check("demo", "script", files, "any")
        make_addon.check("demo", "script", ["init.lua", "data/notes.txt"], "any")


class PackageContentTests(unittest.TestCase):
    def test_make_addon_refuses_out_inside_dir_and_links_out_of_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "src"
            src.mkdir()
            (src / "init.lua").write_text("print('hi')\n")
            (src / ".secret").write_text("key")
            result = run("make_addon.py", "--name", "demo", "--kind", "script", "--version", "1.0.0", "--caps", "read",
                         "--dir", src, "--out", src / "release")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("inside", result.stderr)
            self.assertFalse((src / "release").exists())

            out = Path(tmp) / "out"
            result = run("make_addon.py", "--name", "demo", "--kind", "script", "--version", "1.0.0", "--caps", "read",
                         "--dir", src, "--out", out)
            self.assertEqual(result.returncode, 0, result.stderr)
            with zipfile.ZipFile(out / "demo-1.0.0.zip") as z:
                self.assertEqual(z.namelist(), ["init.lua"])  # the dotfile stays out

            if hasattr(os, "symlink"):
                outside = Path(tmp) / "outside.txt"
                outside.write_text("not for the package")
                os.symlink(outside, src / "data.txt")
                result = run("make_addon.py", "--name", "demo", "--kind", "script", "--version", "1.0.1", "--caps",
                             "read", "--dir", src, "--out", out)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("a link out of", result.stderr)
                os.remove(src / "data.txt")
                os.symlink("init.lua", src / "same.lua")  # inside: stored as the file
                result = run("make_addon.py", "--name", "demo", "--kind", "script", "--version", "1.0.2", "--caps",
                             "read", "--dir", src, "--out", out)
                self.assertEqual(result.returncode, 0, result.stderr)
                with zipfile.ZipFile(out / "demo-1.0.2.zip") as z:
                    self.assertEqual(z.read("same.lua"), b"print('hi')\n")

    def test_make_update_leaves_dotfiles_out_and_refuses_out_inside_dist(self):
        with tempfile.TemporaryDirectory() as tmp:
            dist = Path(tmp) / "dist"
            dist.mkdir()
            (dist / "hailstone").write_text("binary")
            (dist / ".DS_Store").write_text("junk")
            (dist / ".git").mkdir()
            (dist / ".git" / "config").write_text("junk")
            result = run("make_update.py", "--version", "1.0.0", "--platform", "linux-x64", "--dist", dist,
                         "--out", dist / "release")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("inside", result.stderr)
            out = Path(tmp) / "out"
            result = run("make_update.py", "--version", "1.0.0", "--platform", "linux-x64", "--dist", dist,
                         "--out", out)
            self.assertEqual(result.returncode, 0, result.stderr)
            with zipfile.ZipFile(out / "hailstone-1.0.0-linux-x64.zip") as z:
                self.assertEqual(z.namelist(), ["hailstone"])
            result = run("make_update.py", "--version", "1.02.0", "--platform", "linux-x64", "--dist", dist,
                         "--out", out)
            self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
