from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]


class ManagedScriptPackageTests(unittest.TestCase):
    def test_script_activation_is_an_explicit_portable_package_flag(self):
        for managed in (False, True):
            with self.subTest(managed=managed), tempfile.TemporaryDirectory() as tmp:
                source, output = Path(tmp) / "source", Path(tmp) / "output"
                source.mkdir()
                (source / "init.lua").write_text("return 1\n")
                args = [sys.executable, str(ROOT / "tools/make_addon.py"), "--name", "managed", "--kind", "script",
                        "--version", "1.0.0", "--caps", "read,ui", "--dir", str(source), "--out", str(output)]
                if managed:
                    args.append("--autoload")
                subprocess.run(args, check=True, capture_output=True)
                manifest = (output / "hailstone-addon.txt").read_text()
                self.assertEqual("autoload 1\n" in manifest, managed)
                self.assertIn("kind script\n", manifest)
                self.assertIn("package any ", manifest)
                self.assertNotIn("autoupdate", manifest)
                with zipfile.ZipFile(output / "managed-1.0.0.zip") as archive:
                    self.assertEqual(archive.namelist(), ["init.lua"])

    def test_autoload_rejects_each_non_script_kind_before_packaging(self):
        for kind in ("plugin", "tool", "upscaler", "ui"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as tmp:
                output = Path(tmp) / "output"
                result = subprocess.run([sys.executable, str(ROOT / "tools/make_addon.py"), "--name", "managed",
                                         "--kind", kind, "--version", "1.0.0", "--autoload", "--out", str(output)],
                                        text=True, capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("--kind script", result.stderr)
                self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
