// The player's word on a server's compiled plugins (plugin/consent.h): servers/<profile>/plugin_approvals.txt, and
// the hs API the UI packs ask it with (hs.PluginConsent, hs.AnswerPluginConsent; app/lua_ui_plugins.cpp), which a
// plugin's own code in the UI can neither read nor answer.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "core/atomic_file.h"
#include "core/sha256.h"
#include "plugin/consent.h"
#include "plugin/policy.h"
#include "scratch_dir.h"
#include "ui/lua/lua_host.h"

using namespace hailstone;
namespace fs = std::filesystem;

TEST_CASE("Plugin approvals: kept by hash per server, one answer per plugin name, revoked and forgotten by name") {
    test::ScratchDir dir("plugin_approvals");
    const fs::path alpha = dir.path() / "servers" / "alpha", beta = dir.path() / "servers" / "beta";
    const Sha256Digest v1 = sha256(std::string_view("version one")), v2 = sha256(std::string_view("version two"));
    const Sha256Digest other = sha256(std::string_view("another plugin"));

    CHECK(plugin::plugin_consent(alpha, v1) == plugin::Consent::Unasked); // no file
    CHECK(plugin::plugin_consent({}, v1) == plugin::Consent::Unasked);     // no server folder
    CHECK_FALSE(plugin::set_plugin_consent({}, "x", v1, true));

    REQUIRE(plugin::set_plugin_consent(alpha, "MQ2Thing", v1, true));
    REQUIRE(plugin::set_plugin_consent(alpha, "Other Plugin", other, false)); // a name with a space
    CHECK(plugin::plugin_consent(alpha, v1) == plugin::Consent::Allowed);
    CHECK(plugin::plugin_consent(alpha, other) == plugin::Consent::Denied);
    CHECK(plugin::plugin_consent(beta, v1) == plugin::Consent::Unasked); // another server: its own answers
    CHECK(plugin::plugin_consent(alpha, v2) == plugin::Consent::Unasked);  // other bytes: not answered

    // A new version's answer replaces the old one's: the old bytes are not allowed any more.
    REQUIRE(plugin::set_plugin_consent(alpha, "MQ2Thing", v2, true));
    CHECK(plugin::plugin_consent(alpha, v2) == plugin::Consent::Allowed);
    CHECK(plugin::plugin_consent(alpha, v1) == plugin::Consent::Unasked);
    auto all = plugin::read_plugin_approvals(alpha);
    REQUIRE(all.size() == 2);
    CHECK(all[0].name == "Other Plugin");
    CHECK(all[1].name == "MQ2Thing");

    REQUIRE(plugin::revoke_plugin_consent(alpha, "MQ2THING"));
    CHECK(plugin::plugin_consent(alpha, v2) == plugin::Consent::Denied);
    REQUIRE(plugin::forget_plugin_consent(alpha, "mq2thing"));
    CHECK(plugin::plugin_consent(alpha, v2) == plugin::Consent::Unasked);
    CHECK(plugin::plugin_consent(alpha, other) == plugin::Consent::Denied); // the other plugin's stays
}

// Approvals are by hash: the server sending allowed bytes under another name keeps them allowed, so taking the word
// back or forgetting it goes by the plugin's bytes as the server has them now, as well as by its name.
TEST_CASE("Plugin approvals: revoked and forgotten by the plugin's bytes too") {
    test::ScratchDir dir("plugin_approvals_hash");
    const fs::path alpha = dir.path() / "servers" / "alpha";
    const Sha256Digest bytes = sha256(std::string_view("the plugin")), fresh = sha256(std::string_view("unasked"));
    REQUIRE(plugin::set_plugin_consent(alpha, "OldName", bytes, true));
    // The server renamed it: by the new name alone nothing would change.
    REQUIRE(plugin::revoke_plugin_consent(alpha, "NewName"));
    CHECK(plugin::plugin_consent(alpha, bytes) == plugin::Consent::Allowed);
    REQUIRE(plugin::revoke_plugin_consent(alpha, "NewName", {bytes, fresh}));
    CHECK(plugin::plugin_consent(alpha, bytes) == plugin::Consent::Denied);
    CHECK(plugin::plugin_consent(alpha, fresh) == plugin::Consent::Denied); // not answered before: a no now
    REQUIRE(plugin::forget_plugin_consent(alpha, "NewName", {bytes}));
    CHECK(plugin::plugin_consent(alpha, bytes) == plugin::Consent::Unasked);
    CHECK(plugin::plugin_consent(alpha, fresh) == plugin::Consent::Unasked); // (its line had the name NewName)
}

TEST_CASE("Plugin approvals: a file this version does not read allows nothing; bad lines are skipped") {
    test::ScratchDir dir("plugin_approvals_bad");
    const Sha256Digest h = sha256(std::string_view("bytes"));
    REQUIRE(write_file_atomic(dir.path() / plugin::kApprovalsName, "hailstone-plugin-approvals 9\nallow " + to_hex(h) + " X\n"));
    CHECK(plugin::plugin_consent(dir.path(), h) == plugin::Consent::Unasked);
    REQUIRE(write_file_atomic(dir.path() / plugin::kApprovalsName,
                              "# comment\nhailstone-plugin-approvals 1\nallow nothex X\nmaybe " + to_hex(h) +
                                  " X\nallow " + to_hex(h) + "\n\nallow " + to_hex(h) + " Good\r\n"));
    auto all = plugin::read_plugin_approvals(dir.path());
    REQUIRE(all.size() == 1);
    CHECK(all[0].name == "Good");
    CHECK(all[0].allowed);
}

namespace {

std::string run(lua_State* L, const std::string& code) {
    if (luaL_dostring(L, code.c_str()) != LUA_OK) {
        std::string e = lua_tostring(L, -1);
        lua_settop(L, 0);
        return "error: " + e;
    }
    std::string out = lua_isnil(L, -1) ? "nil" : luaL_tolstring(L, -1, nullptr);
    lua_settop(L, 0);
    return out;
}

// Runs code in a plugin folder's own globals (ui::lua::LuaUi::set_folders).
std::string in_folder(ui::lua::LuaUi& lua, const char* tag, const std::string& code) {
    lua_State* L = lua.state();
    if (luaL_loadstring(L, code.c_str()) != LUA_OK) return "error: load";
    lua.push_folder_env_for_test(tag);
    if (lua_isnil(L, -1)) {
        lua_settop(L, 0);
        return "error: no such folder";
    }
    lua_setupvalue(L, -2, 1);
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        std::string e = lua_tostring(L, -1);
        lua_settop(L, 0);
        return "error: " + e;
    }
    std::string out = lua_isnil(L, -1) ? "nil" : luaL_tolstring(L, -1, nullptr);
    lua_settop(L, 0);
    return out;
}

// A host with a question about one plugin, and the answers it was given.
struct Asking {
    std::optional<plugin::ConsentPrompt> prompt;
    std::vector<std::optional<bool>> answers;
    void wire(app::UiHost& host) {
        plugin::ConsentPrompt p;
        p.server = "Alpha";
        p.serial = 7;
        p.files.push_back({"MQ2Thing", fs::path("servers") / "alpha" / "plugins" / "MQ2Thing.so", 123456,
                           sha256(std::string_view("bytes"))});
        prompt = p;
        host.plugin_consent = [this] { return prompt; };
        host.answer_plugin_consent = [this](std::optional<bool> a) {
            answers.push_back(a);
            prompt.reset();
        };
    }
};

} // namespace

TEST_CASE("hs.PluginConsent and hs.AnswerPluginConsent: the question for the packs, and the player's answer") {
    app::UiHost host;
    app::UiApi api(host);
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    lua_newtable(L);
    api.register_functions(L);
    lua_setglobal(L, "hs");

    CHECK(run(L, "return hs.PluginConsent()") == "nil"); // no host function: nothing asked
    CHECK(run(L, "return hs.AnswerPluginConsent(true, 7)") == "false");
    Asking a;
    a.wire(host);
    const std::string hex = to_hex(sha256(std::string_view("bytes")));
    CHECK(run(L, "return hs.PluginConsent().server") == "Alpha");
    CHECK(run(L, "return hs.PluginConsent().serial") == "7");
    CHECK(run(L, "return hs.PluginConsent().warning") == std::string(plugin::kConsentWarning));
    CHECK(run(L, "return #hs.PluginConsent().plugins") == "1");
    CHECK(run(L, "return hs.PluginConsent().plugins[1].name") == "MQ2Thing");
    CHECK(run(L, "return hs.PluginConsent().plugins[1].file") == "MQ2Thing.so");
    CHECK(run(L, "return hs.PluginConsent().plugins[1].size") == "123456");
    CHECK(run(L, "return hs.PluginConsent().plugins[1].sha256") == hex);
    CHECK(run(L, "return hs.PluginConsent().plugins[1].short") == hex.substr(0, 16));
    CHECK(run(L, "return (pcall(hs.AnswerPluginConsent, 'yes', 7))") == "false"); // a boolean or nil only
    CHECK(run(L, "return (pcall(hs.AnswerPluginConsent, true))") == "false");     // the question's serial is required
    CHECK(run(L, "return hs.AnswerPluginConsent(true, 8)") == "false");           // another question's: refused
    CHECK(a.answers.empty());
    CHECK(run(L, "return hs.AnswerPluginConsent(nil, 7)") == "true");
    REQUIRE(a.answers.size() == 1);
    CHECK_FALSE(a.answers[0].has_value());
    CHECK(run(L, "return hs.AnswerPluginConsent(true, 7)") == "false"); // no question now
    a.wire(host);
    CHECK(run(L, "return hs.AnswerPluginConsent(false, hs.PluginConsent().serial)") == "true");
    a.wire(host);
    a.prompt->serial = 9; // a new question came up after the pack read serial 7
    CHECK(run(L, "return hs.AnswerPluginConsent(true, 7)") == "false");
    CHECK(run(L, "return hs.AnswerPluginConsent(true, 9)") == "true");
    REQUIRE(a.answers.size() == 3);
    CHECK(a.answers[1] == std::optional<bool>(false));
    CHECK(a.answers[2] == std::optional<bool>(true));
    lua_close(L);
}

// A server's own plugin windows run in the UI as that plugin: they must not see the question nor say yes to it.
TEST_CASE("hs.PluginConsent in a plugin's code: nil, and its answer is refused") {
    test::ScratchDir dir("plugin_consent_confined");
    const fs::path pack = dir.path() / "pack", plug = dir.path() / "plug";
    fs::create_directories(pack);
    fs::create_directories(plug);
    std::ofstream(pack / "consent.lua") << "ui.window('Consent', function() pack_saw = hs.PluginConsent() ~= nil end)\n";
    std::ofstream(plug / "sneak.lua") << "at_load = hs.AnswerPluginConsent(true, 7)\n"
                                         "ui.window('Sneak', function()\n"
                                         "  saw = hs.PluginConsent() ~= nil\n"
                                         "  in_draw = hs.AnswerPluginConsent(true, 7)\n"
                                         "end)\n";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        app::UiHost host;
        app::UiApi api(host);
        Asking a;
        a.wire(host);
        ui::lua::LuaUi lua(pack, dir.path() / "ui.txt");
        lua.add_ui_api([&](lua_State* L) { api.register_functions(L); });
        lua.set_caller_scope([&](std::string_view tag) -> std::shared_ptr<void> {
            return std::make_shared<plugin::CallerScope>(plugin::kCapAll, tag);
        });
        lua.set_folders({{"plug", plug}});
        ImGui::GetIO().DisplaySize = ImVec2(800, 600);
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        lua.frame();
        ImGui::Render();
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        CHECK(run(lua.state(), "return pack_saw") == "true"); // the pack's own window sees it
        CHECK(in_folder(lua, "plug", "return saw") == "false");
        CHECK(in_folder(lua, "plug", "return at_load") == "false");
        CHECK(in_folder(lua, "plug", "return in_draw") == "false");
        CHECK(a.answers.empty()); // the plugin answered nothing
        CHECK(a.prompt.has_value());
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("plugin consent: each write of the answers changes their generation, and answers read once can be asked") {
    test::ScratchDir dir("plugin_consent_generation");
    const auto a = sha256(std::string_view("plugin a")), b = sha256(std::string_view("plugin b"));
    const auto before = plugin::plugin_approvals_generation();
    REQUIRE(plugin::set_plugin_consent(dir.path(), "A", a, true));
    const auto after_allow = plugin::plugin_approvals_generation();
    CHECK(after_allow != before);
    REQUIRE(plugin::set_plugin_consent(dir.path(), "B", b, false));
    CHECK(plugin::plugin_approvals_generation() != after_allow);
    const auto answers = plugin::read_plugin_approvals(dir.path());
    CHECK(plugin::consent_in(answers, a) == plugin::Consent::Allowed);
    CHECK(plugin::consent_in(answers, b) == plugin::Consent::Denied);
    CHECK(plugin::consent_in(answers, sha256(std::string_view("other"))) == plugin::Consent::Unasked);
    CHECK(plugin::consent_in(answers, a) == plugin::plugin_consent(dir.path(), a));
}
