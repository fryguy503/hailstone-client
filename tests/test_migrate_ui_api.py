import importlib.util
from pathlib import Path
import re
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("migrate_ui_api", ROOT / "tools/migrate_ui_api.py")
migration = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = migration
spec.loader.exec_module(migration)
MAPPING = {"InGame": ("session", "InGame"), "ShowWindow": ("windows", "Show"),
           "gamepad.set_mode": ("gamepad", "SetMode"), "CursorSlot": ("inventory", "CursorSlot")}


class MigrationTests(unittest.TestCase):
    def test_comments_strings_and_owned_fields_are_unchanged(self):
        before = '''-- hs.InGame() in prose
--[=[ hs.ShowWindow("InventoryWindow") ]=]
local label = "hs.InGame()"
local escaped = 'a\\\' hs.ShowWindow'
local block = [==[hs.gamepad.set_mode("menu")]==]
local object = thing.hs.InGame
if hs.InGame() then hs.ShowWindow("InventoryWindow", true) end
'''
        after = migration.migrate(before, MAPPING)
        self.assertIn('-- hs.InGame() in prose', after)
        self.assertIn('local label = "hs.InGame()"', after)
        self.assertIn('local block = [==[hs.gamepad.set_mode("menu")]==]', after)
        self.assertIn('local object = thing.hs.InGame', after)
        self.assertIn('if API.session.InGame() then API.windows.Show("InventoryWindow", true) end', after)
        self.assertEqual(after.count("local API = hs.UI"), 1)
        self.assertEqual(migration.migrate(after, MAPPING), after)

    def test_nested_methods_constants_spacing_and_line_endings(self):
        before = '-- title\r\nlocal slot = hs . CursorSlot\r\nhs.gamepad.set_mode("menu")\r\nlocal g = hs.gamepad\r\n'
        after = migration.migrate(before, MAPPING)
        self.assertIn('local API = hs.UI\r\n', after)
        self.assertIn('API.inventory.CursorSlot', after)
        self.assertIn('API.gamepad.SetMode("menu")', after)
        self.assertIn('local g = API.gamepad', after)
        self.assertNotIn('\n', after.replace('\r\n', ''))

    def test_unknown_calls_and_conflicting_alias_require_review(self):
        with self.assertRaisesRegex(ValueError, "Deprecated UI namespace"):
            migration.migrate('local API = eq.UI\nAPI.session.InGame()', MAPPING)
        with self.assertRaisesRegex(ValueError, "Unmapped binding"):
            migration.migrate('hs.NotARealApi()', MAPPING)
        with self.assertRaisesRegex(ValueError, "Unmapped binding"):
            migration.migrate('hs.gamepad.not_a_real_method()', MAPPING)
        with self.assertRaisesRegex(ValueError, "already used"):
            migration.migrate('local API = {}\nhs.InGame()', MAPPING)
        with self.assertRaisesRegex(ValueError, "Unterminated"):
            migration.migrate('local text = [=[oops', MAPPING)

    def test_catalog_is_the_explicit_method_and_constant_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "api.inc"
            path.write_text('UI_METHOD("InGame", "session", "InGame", 0)\n'
                            'UI_CONSTANT("CursorSlot", "inventory", "CursorSlot")\n')
            self.assertEqual(migration.catalog(path), {"InGame": MAPPING["InGame"], "CursorSlot": MAPPING["CursorSlot"]})
            path.write_text(path.read_text() + 'UI_METHOD("InGame", "session", "Other", 0)\n')
            with self.assertRaisesRegex(ValueError, "Duplicate"):
                migration.catalog(path)

    def test_mock_discovery_versions_match_the_published_native_contract(self):
        source = (ROOT / "src/app/lua_ui_api.cpp").read_text(encoding="utf-8")
        version = source.split("void push_version(lua_State* L)", 1)[1].split("void push_member_description", 1)[0]
        fields = dict((field, int(number)) for number, field in re.findall(
            r'lua_pushinteger\(L,\s*(\d+)\);\s*lua_setfield\(L,\s*-2,\s*"(major|minor)"\)', version))
        self.assertEqual(set(fields), {"major", "minor"})
        adapter = migration.mock_adapter(ROOT / "src/app/lua_ui_api.inc")
        published = (fields["major"], fields["minor"])
        mock_versions = re.findall(r"version=\{major=(\d+),minor=(\d+)\}", adapter)
        self.assertEqual(len(mock_versions), 2, "Both API.version and Describe().version must be present")
        self.assertEqual({tuple(map(int, values)) for values in mock_versions}, {published})


if __name__ == "__main__":
    unittest.main()
