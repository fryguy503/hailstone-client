import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("gen_ui_api_docs", ROOT / "tools/gen_ui_api_docs.py")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class ReferenceTests(unittest.TestCase):
    def test_typed_extension_registration_resolves_addressed_functions_without_foreign_pair_tables(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            app = root / "src/app"
            app.mkdir(parents=True)
            (app / "lua_ui_api.inc").write_text(
                'UI_METHOD("ExtensionDrawSection", "extensions", "DrawSection", 0)\n'
                'UI_METHOD("ForeignPair", "extensions", "ForeignPair", 0)\n'
                'UI_METHOD("Unregistered", "extensions", "Unregistered", 0)\n', encoding="utf-8")
            (app / "lua_ui_extensions.cpp").write_text('''// Draw a declared window section.
int DrawSection(lua_State* L) { return 1; }
int Decoy(lua_State* L) { return 0; }
void register_ui_extensions(lua_State* L, const UiCallbackContext& context) {
    for (const auto& binding : std::initializer_list<std::pair<const char*, lua_CFunction>>{
            {"ExtensionDrawSection", &DrawSection}}) {
        lua_pushcclosure(L, binding.second, 1);
        lua_setfield(L, root, binding.first);
    }
    const std::pair<const char*, lua_CFunction> foreign[] = {{"ForeignPair", &Decoy}};
    // {"Unregistered", &Decoy}
    const char* text = R"tag( {"Unregistered", &Decoy} )tag";
}
''', encoding="utf-8")
            report = {}
            rendered = generator.render(root, report)
            self.assertEqual(report["verified_method_bindings"], 1)
            self.assertEqual(report["unresolved_methods"],
                             ["API.extensions.ForeignPair", "API.extensions.Unregistered"])
            self.assertIn('[source](../src/app/lua_ui_extensions.cpp#L2)', rendered)
            self.assertIn('Draw a declared window section.', rendered)
            self.assertIn('`hs.ForeignPair` · **not located**', rendered)
            self.assertIn('`hs.Unregistered` · **not located**', rendered)

    def test_hook_policy_requires_a_unique_method_and_is_visible_in_reference(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            app = root / "src/app"
            app.mkdir(parents=True)
            catalog = app / "lua_ui_api.inc"
            rows = ('UI_METHOD("Read", "session", "Read", 0)\n'
                    'UI_CONSTANT("Value", "session", "Value")\n')
            catalog.write_text(rows + 'UI_HOOK_POLICY("Read")\n', encoding="utf-8")
            (app / "lua_ui_sample.cpp").write_text('''
int Read(lua_State* L) { return 0; }
void register_sample_api(lua_State* L) {
  static const luaL_Reg functions[] = {{"Read", Read}};
  lua_pushinteger(L, 1);lua_setfield(L, -2, "Value");
}
''', encoding="utf-8")
            rendered = generator.render(root)
            self.assertIn('| none (outer gate) | plain-capabilities |', rendered)
            for policies in ('UI_HOOK_POLICY("Read")\nUI_HOOK_POLICY("Read")\n',
                             'UI_HOOK_POLICY("Value")\n', 'UI_HOOK_POLICY("Unknown")\n'):
                catalog.write_text(rows + policies, encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "UI hook policy"):
                    generator.render(root)

    def test_public_registration_controls_links_and_unknowns_stay_explicit(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            app = root / "src/app"
            app.mkdir(parents=True)
            (app / "lua_ui_api.inc").write_text(
                'UI_METHOD("Read", "session", "Read", 0)\n'
                'UI_METHOD("Alias", "session", "Alias", 0)\n'
                'UI_METHOD("Direct", "session", "Direct", kCapCommands | plugin::kCapUi)\n'
                'UI_METHOD("Bare", "session", "Bare", 0)\n'
                'UI_METHOD("Decoy", "session", "Decoy", 0)\n'
                'UI_METHOD("gamepad.down", "gamepad", "Down", kCapActions)\n'
                'UI_CONSTANT("CursorSlot", "inventory", "CursorSlot")\n', encoding="utf-8")
            core = '''// value = hs.Read(name) -- nil if absent.
int InternalRead(lua_State* L) { return 1; }
// This distant prose is not adjacent to Bare.

int Bare(lua_State* L) { return 0; }
// Unregistered helper with a misleading public name.
int Decoy(lua_State* L) { return 0; }
int Down(lua_State* L) { return 0; }
void UiApi::register_functions(lua_State* L) {
    static const luaL_Reg methods[] = {{"Read", InternalRead}, {"Alias", InternalRead}};
    luaL_setfuncs(L, methods, 1);
    for (auto [name, fn] : std::initializer_list<std::pair<const char*, lua_CFunction>>{{"Bare", Bare}}) {
        lua_pushcclosure(L, fn, 1);
        lua_setfield(L, -2, name);
    }
    const OtherType noise[] = {{"Decoy", Decoy}};
    // {"Decoy", Decoy}
    const char* text = R"tag( } {"Decoy", Decoy} )tag";
    lua_pushcclosure(L, DirectImpl, 1);
    lua_setfield(L, -2, "Direct");
    lua_pushinteger(L, 33);
    lua_setfield(L, -2, "CursorSlot");
}
'''
            gamepad = '''// held = hs.gamepad.down(button)
int Down(lua_State* L) { return 1; }
void register_gamepad_api(lua_State* L, UiHost& host) {
    static const luaL_Reg fns[] = {{"down", Down}};
    luaL_setfuncs(L, fns, 1);
    lua_setfield(L, -2, "gamepad");
}
'''
            other = '''// Returns a `ready` flag or nil.
int DirectImpl(lua_State* L) { return 1; }
'''
            for name, text in (("lua_ui.cpp", core), ("lua_ui_gamepad.cpp", gamepad), ("lua_ui_other.cpp", other)):
                (app / name).write_text(text, encoding="utf-8")
            (app / "lua_ui_api.cpp").write_text('''int Decoy(lua_State* L) { return 0; }
void register_ui_api(lua_State* L) {
    static const luaL_Reg fns[] = {{"Decoy", Decoy}};
}
''', encoding="utf-8")
            report = {}
            result = generator.render(root, report)
            self.assertEqual(result, generator.render(root))
            self.assertEqual(report["methods"], 6)
            self.assertEqual(report["constants"], 1)
            self.assertEqual(report["verified_constant_bindings"], 1)
            self.assertEqual(report["verified_method_bindings"], 5)
            self.assertEqual(report["documented_excerpts"], 4)  # The alias shares its implementation's comment.
            self.assertEqual(report["unresolved_methods"], ["API.session.Decoy"])
            self.assertEqual(report["undocumented_methods"], ["API.session.Bare", "API.session.Decoy"])
            self.assertIn('value = hs.Read(name) -- nil if absent.', result)
            self.assertIn('held = hs.gamepad.down(button)', result)
            self.assertIn('[source](../src/app/lua_ui_gamepad.cpp#L2)', result)
            self.assertIn('[source](../src/app/lua_ui_other.cpp#L2)', result)
            self.assertIn('| `API.session.Bare` | **See binding** |', result)
            self.assertIn('| `API.session.Decoy` | **See binding** | `hs.Decoy` · **not located** |', result)
            self.assertIn('| `API.inventory.CursorSlot` | `= 33` |', result)
            self.assertIn('| commands, ui |', result)
            self.assertNotIn('This distant prose', result)

    def test_comment_string_and_unexported_nested_registrars_do_not_create_bindings(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            app = root / "src/app"
            app.mkdir(parents=True)
            (app / "lua_ui.cpp").write_text('''
/* int Commented(lua_State* L) { }
void register_comment_api(lua_State* L) {
    static const luaL_Reg fns[] = {{"Ghost", Commented}};
} */
const char* text = R"tag(int StringOnly(lua_State* L) { }
void register_string_api(lua_State* L) {
    static const luaL_Reg fns[] = {{"Ghost", StringOnly}};
})tag";
int Real(lua_State* L) { return 0; }
void register_gamepad_api(lua_State* L, UiHost& host) {
    static const luaL_Reg fns[] = {{"down", Real}};
    // lua_setfield(L, -2, "gamepad");
}
''', encoding="utf-8")
            self.assertEqual(generator.implementation_index(root), ({}, {}))

    def test_inline_comments_render_as_code_even_with_backticks_and_table_pipes(self):
        self.assertEqual(generator.code('`flag` is "a" | "b"'), '`` `flag` is "a" \\| "b" ``')


if __name__ == "__main__":
    unittest.main()
