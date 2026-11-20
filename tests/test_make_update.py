import importlib.util
from pathlib import Path
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
                  "ui-addons/skin/ui/dark/ui.ini", "UI-ADDONS/skin/ui/dark/window.lua", "ui-addons"):
            self.assertTrue(make_update.is_player_path(p), p)
        for p in ("hailstone", "hailstone.exe", "shaders/a.spv", "ui/modern/login.lua", "browser/libcef.so",
                  "plugins/nms/nms.so", "icons/hailstone.png", "servers.txt", "ui-data-copy/file.lua",
                  "ui-database/file.lua", "ui-data.lua", "ui-addons.lua"):
            self.assertFalse(make_update.is_player_path(p), p)


if __name__ == "__main__":
    unittest.main()
