import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("make_addon", ROOT / "tools/make_addon.py")
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)


class UiPackageTests(unittest.TestCase):
    def test_ui_cli_creates_portable_metadata_package_without_script_bootstrap(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, output = Path(tmp) / "source", Path(tmp) / "output"
            (source / "ui/example").mkdir(parents=True)
            (source / "ui/example/ui.ini").write_text("name=Example\nbase=builtin:modern\n")
            (source / "LICENSE").write_text("original fixture license")
            subprocess.run([sys.executable, str(ROOT / "tools/make_addon.py"), "--name", "interfaces", "--kind", "ui",
                            "--version", "1.0.0", "--dir", str(source), "--out", str(output)],
                           check=True, capture_output=True)
            manifest = (output / "hailstone-addon.txt").read_text()
            self.assertIn("kind ui\n", manifest)
            self.assertIn("caps none\n", manifest)
            self.assertIn("package any ", manifest)
            with zipfile.ZipFile(output / "interfaces-1.0.0.zip") as archive:
                self.assertEqual(archive.namelist(), ["LICENSE", "ui/example/ui.ini"])

    def test_ui_scope_rejects_bootstrap_programs_missing_manifests_and_case_collisions(self):
        for paths in (["init.lua"], ["ui/example/panel.lua"], ["ui/example/ui.ini", "ui/other/panel.lua"],
                      ["ui/example/ui.ini", "ui/example/run.exe"], ["ui/example/ui.ini", "ui/example/lib.so.1"],
                      ["ui/example/ui.ini", "ui/EXAMPLE/panel.lua"], ["ui/bad.name/ui.ini"],
                      ["ui/example/ui.ini", "ui/example/lib/a.lua", "ui/example/Lib/b.lua"],
                      ["ui/example/ui.ini", "ui/example/lib", "ui/example/lib/a.lua"],
                      ["ui/example/ui.ini", "ui/example/lib/a.lua", "ui/example/lib"]):
            with self.subTest(paths=paths), self.assertRaises(SystemExit):
                packager.check("interfaces", "ui", paths, "any")

    def test_ui_nested_helpers_share_a_consistent_portable_directory(self):
        packager.check("interfaces", "ui", ["ui/example/ui.ini", "ui/example/lib/a.lua",
                                            "ui/example/lib/b.lua", "ui/example/lib/deep/c.lua"], "any")


if __name__ == "__main__":
    unittest.main()
