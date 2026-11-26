// The shipped UIs over Dear ImGui, part 3 (part 1, tests/ui_pack_windows_test.cpp, says what they cover).
#include <doctest/doctest.h>

#include "ui_pack_session.h"

TEST_CASE("UI packs: server shroud catalogs and storage draw in both packs") {
    for (const char* pack : {"modern", "gamepad"}) {
        CAPTURE(pack);
        test::ScratchDir scratch("ui_pack_shroud_test");
        const auto dir = scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(fs::path(__FILE__).parent_path().parent_path() / "ui" / pack, dir / pack,
                 fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            test::Rig rig;
            REQUIRE(rig.to_zone());
            Session s(dir, pack);
            s.host.conn = &rig.conn;
            lua_State* L = s.lua.state();
            auto run = [&](const char* code) {
                INFO(code);
                const int result = luaL_dostring(L, code);
                INFO((lua_tostring(L, -1) ? lua_tostring(L, -1) : ""));
                REQUIRE(result == LUA_OK);
                lua_settop(L, 0);
            };
            auto offer = [&](std::uint32_t context) {
                ByteWriter payload;
                payload.le(context);
                payload.zeros(8);
                net::detail::write_cstring(payload, "PROGRESSION\a" "12\a" "Scouts\a" "Scout forms\a"
                    "BRANCH\a" "44\a" "Trackers\a" "Track a target\a" "50\a"
                    "TEMPLATE\a" "Kobold\a" "391\a" "20\a" "1\a"
                    "TEMPLATE\a" "Gnoll\a" "392\a" "25\a" "0");
                rig.conn.receive_zone_packet_for_test(net::make_packet(net::Op::ShroudSelectionWindow, std::move(payload)));
            };
            auto drawn = [&](std::string title, std::string_view screen) {
                if (std::string_view(pack) == "modern") title += "###modern." + std::string(screen);
                const auto* window = ImGui::FindWindowByName(title.c_str());
                return window && window->WasActive;
            };
            s.frames(2);
            CHECK_FALSE(s.api.shown("ProgressionSelectionWnd"));
            offer(0);
            s.frames(2);
            CHECK(s.api.shown("ProgressionSelectionWnd"));
            CHECK(s.api.replaces("ProgressionSelectionWnd"));
            CHECK(drawn("Spirit Shroud Selection", "ProgressionSelectionWnd"));
            run(R"lua(
                local screen = "ProgressionSelectionWnd"
                local windows = hs.UI.windows
                assert(windows.ListInfo(screen, "Forms") == 2)
                assert(windows.ListRow(screen, "Forms", 0)[2] == "Kobold")
                assert(windows.ListClick(screen, "Forms", 1, "left"))
                assert(windows.ChildState(screen, "Accept") == false)
                windows.Show(screen, false)
            )lua");
            CHECK_FALSE(s.api.shown("ProgressionSelectionWnd"));

            game::Task mission;
            mission.task_id = 926;
            mission.title = "Scout the ruins";
            mission.type = game::kTaskTypeShared;
            rig.conn.receive_zone_packet_for_test(net::encode_task_description(mission));
            offer(mission.task_id);
            s.frames(2);
            CHECK(s.api.shown("TaskTemplateSelectWnd"));
            CHECK(s.api.replaces("TaskTemplateSelectWnd"));
            CHECK(drawn("Monster Mission Selection", "TaskTemplateSelectWnd"));
            run(R"lua(hs.UI.windows.Show("TaskTemplateSelectWnd", false))lua");
            CHECK_FALSE(s.api.shown("TaskTemplateSelectWnd"));

            ByteWriter roles;
            roles.zeros(8);
            roles.le(mission.task_id);
            roles.zeros(5);
            roles.le<std::uint32_t>(1);
            roles.le<std::uint32_t>(913);
            roles.le<std::int32_t>(1);
            roles.le<std::int32_t>(1);
            roles.zeros(4);
            roles.le<std::uint8_t>(1);
            net::detail::write_cstring(roles, "Watch Captain");
            rig.conn.receive_zone_packet_for_test(net::make_packet(net::Op::MonsterMissionCatalog, std::move(roles)));
            s.frames(2);
            CHECK(drawn("Monster Mission Selection", "TaskTemplateSelectWnd"));
            run(R"lua(
                local screen = "TaskTemplateSelectWnd"
                local windows = hs.UI.windows
                local role = windows.ListRow(screen, "Forms", 0)
                assert(role[1] == "Required" and role[2] == "Watch Captain" and role[5] == "0 / 1")
                assert(windows.ListClick(screen, "Forms", 0, "left"))
                assert(windows.ChildState(screen, "Accept"))
                assert(windows.ClickChild(screen, "Accept", "left"))
                assert(windows.ChildState(screen, "Accept") == false)
            )lua");
            ByteWriter refused;
            refused.zeros(8);
            refused.le<std::uint32_t>(913);
            refused.le<std::uint32_t>(1);
            rig.conn.receive_zone_packet_for_test(net::make_packet(net::Op::MonsterMissionResult, std::move(refused)));
            run(R"lua(assert(hs.UI.windows.ChildState("TaskTemplateSelectWnd", "Accept")))lua");
            rig.conn.receive_zone_packet_for_test(net::encode(std::vector<net::SharedTaskMember>{{"Helper", true, 0}}));
            ByteWriter assignment;
            assignment.zeros(8);
            assignment.fixed_string("Helper", 64);
            assignment.le<std::uint32_t>(913);
            rig.conn.receive_zone_packet_for_test(net::make_packet(net::Op::MonsterMissionAssignment, std::move(assignment)));
            s.frames(2);
            run(R"lua(
                local screen = "TaskTemplateSelectWnd"
                local windows = hs.UI.windows
                local role = windows.ListRow(screen, "Forms", 0)
                assert(role[4] == "Full" and role[5] == "1 / 1")
                assert(windows.ChildState(screen, "Accept") == false)
                windows.Show(screen, false)
            )lua");

            ByteWriter progress;
            progress.le<std::uint32_t>(1);
            constexpr std::string_view progress_text = "PROGRESSION\a" "12\a" "Scouts\a" "Scout forms\a"
                "1\a" "2\a" "Gnoll\a" "Gnoll form\a" "50";
            progress.bytes(std::as_bytes(std::span(progress_text.data(), progress_text.size())));
            rig.conn.receive_zone_packet_for_test(net::make_packet(net::Op::ShroudProgress, std::move(progress)));
            run(R"lua(
                local windows = hs.UI.windows
                assert(windows.ChildShown("InventoryWindow", "ShroudProgressButton") == true)
                assert(windows.ClickChild("InventoryWindow", "ShroudProgressButton", "left"))
            )lua");
            s.frames(2);
            CHECK(drawn("Spirit Shroud Progress", "ShroudProgressWnd"));
            run(R"lua(hs.UI.windows.Show("ShroudProgressWnd", false))lua");

            REQUIRE(rig.conn.profile());
            net::Spawn banker;
            banker.spawn_id = 133;
            banker.name = "Shroud Banker";
            banker.npc = 1;
            banker.class_ = 40;
            banker.motion.x = rig.conn.profile()->x;
            banker.motion.y = rig.conn.profile()->y;
            banker.motion.z = rig.conn.profile()->z;
            rig.conn.receive_zone_packet_for_test(net::encode(banker));
            ByteWriter open;
            open.le<std::uint32_t>(0);
            open.le(banker.spawn_id);
            rig.conn.receive_zone_packet_for_test(net::make_packet(net::Op::AltStorage, std::move(open)));
            ByteWriter items;
            items.le<std::uint32_t>(3600);
            items.le<std::uint32_t>(1);
            net::detail::write_cstring(items, "Tracker's Ledger");
            items.le<std::uint32_t>(431);
            items.le<std::uint64_t>(937);
            items.le<std::uint32_t>(2);
            rig.conn.receive_zone_packet_for_test(net::make_packet(net::Op::AltStorageList, std::move(items)));
            s.frames(2);
            CHECK(s.api.shown("AltStorageWnd"));
            CHECK(s.api.replaces("AltStorageWnd"));
            CHECK(drawn("Shroud Bank", "AltStorageWnd"));
            run(R"lua(
                local windows = hs.UI.windows
                local screen = "AltStorageWnd"
                assert(windows.ListRow(screen, "ItemList", 0)[1] == "Tracker's Ledger")
                assert(windows.Text(screen, "TimerLabel") == "01:00:00")
                assert(windows.ChildState(screen, "GetButton") == false)
            )lua");
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("UI packs: the server plugins' question draws above the loading cover in each pack") {
    // In the world, and on the loading screen, where a required plugin's question holds the zone-in (no game or
    // character UI is ready there).
    for (const bool in_game : {true, false})
    for (const char* pack : {"modern", "gamepad"}) {
        CAPTURE(pack);
        CAPTURE(in_game);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch(std::string("ui_pack_consent_") + pack + (in_game ? "_game" : "_load"));
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            Session s(dir, pack, {}, in_game);
            std::optional<plugin::ConsentPrompt> prompt;
            s.host.plugin_consent = [&] { return prompt; };
            s.host.answer_plugin_consent = [&](std::optional<bool>) { prompt.reset(); };
            ui::LoadingView loading;
            loading.title = "Checking required add-ons";
            loading.detail = "Review the required plugin's permission request to continue.";
            auto consent_window = [&]() -> ImGuiWindow* {
                for (ImGuiWindow* win : ImGui::GetCurrentContext()->Windows)
                    if (win->WasActive && (std::string(win->Name).find("###modern.PluginConsent") != std::string::npos ||
                                           std::string(win->Name) == "Server Plugins"))
                        return win;
                return nullptr;
            };
            auto draw_index = [](const ImDrawList* list) {
                const ImDrawData* data = ImGui::GetDrawData();
                for (int i = 0; i < data->CmdLists.Size; ++i)
                    if (data->CmdLists[i] == list) return i;
                return -1;
            };
            auto foreground_cover = [&] {
                const ImDrawList* foreground = ImGui::GetForegroundDrawList();
                CHECK_FALSE(foreground->VtxBuffer.empty());
                CHECK(draw_index(foreground) == ImGui::GetDrawData()->CmdLists.Size - 1);
                CHECK(ImGui::GetBackgroundDrawList()->VtxBuffer.empty());
            };
            s.frames(2, nullptr, &loading);
            CHECK(s.api.replaces("PluginConsent")); // the client's own prompt stays off
            CHECK(consent_window() == nullptr);
            foreground_cover();
            plugin::ConsentPrompt p;
            p.server = "Alpha";
            p.serial = 1;
            p.files.push_back({"MQ2Thing", "MQ2Thing.so", 2u * 1024 * 1024, {}});
            p.files.push_back({"MQ2Other", "MQ2Other.so", 4096, {}});
            prompt = p;
            s.frames(3, nullptr, &loading);
            ImGuiWindow* consent = consent_window();
            REQUIRE(consent);
            // The loading screen is a window over every other one (in the world: the pack's windows, drawn before it),
            // beneath the question alone; nothing goes in the foreground list over the question.
            ImGuiWindow* cover = ImGui::FindWindowByName(ui::kLoadingCoverWindow);
            REQUIRE(cover);
            CHECK_FALSE(cover->DrawList->VtxBuffer.empty());
            CHECK(draw_index(consent->DrawList) > draw_index(cover->DrawList));
            int beneath = 0;
            for (ImGuiWindow* win : ImGui::GetCurrentContext()->Windows) {
                if (win == cover || win->RootWindow == consent || !win->WasActive || win->DrawList->VtxBuffer.empty() ||
                    draw_index(win->DrawList) < 0)
                    continue;
                CAPTURE(win->Name);
                CHECK(draw_index(win->DrawList) < draw_index(cover->DrawList));
                ++beneath;
            }
            if (in_game) CHECK(beneath > 0); // (the pack draws its game windows; the check above has something to do)
            CHECK(ImGui::GetForegroundDrawList()->VtxBuffer.empty());
            // The mouse anywhere but the question is over the cover, not the windows beneath it.
            ImGui::GetIO().AddMousePosEvent(consent->Pos.x > 40.0f ? 5.0f : 1595.0f, 895.0f);
            s.frames(1, nullptr, &loading);
            CHECK(ImGui::GetCurrentContext()->HoveredWindow == cover);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            prompt.reset();
            s.frames(2, nullptr, &loading);
            CHECK(consent_window() == nullptr);
            foreground_cover();
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

// A required add-on that failed in the world (docs/plugins.md "Opt-in add-on policies"): each pack draws the Server
// add-ons warning in the client's place ("AddonPolicy" replaced) only while there is one, with a close box, and its
// Retry calls the client's with that failure's serial; no Retry where the failure cannot be retried.
TEST_CASE("UI packs: the Server add-ons warning shows only for a failure, can be closed, and Retry reaches the client") {
    for (const char* pack : {"modern", "gamepad"}) {
        CAPTURE(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch(std::string("ui_pack_addon_policy_") + pack);
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            Session s(dir, pack);
            ui::AddonPolicyView view;
            int retries = 0;
            s.host.addon_policy = &view;
            s.host.retry_required_addons = [&] { ++retries; };
            auto window = [&]() -> ImGuiWindow* {
                for (ImGuiWindow* win : ImGui::GetCurrentContext()->Windows)
                    if (win->WasActive && (std::string(win->Name).find("###modern.AddonPolicy") != std::string::npos ||
                                           std::string(win->Name) == "Server add-ons"))
                        return win;
                return nullptr;
            };
            lua_State* L = s.lua.state();
            REQUIRE(luaL_dostring(L, R"lua(
local button, invisible = ui.Button, ui.InvisibleButton
retryRect = nil
ui.Button = function(label, ...)
  local hit = button(label, ...)
  if label == "Retry##addonpolicy" then retryRect = { ui.GetItemRect() } end
  return hit
end
ui.InvisibleButton = function(id, ...)
  local hit = invisible(id, ...)
  if id == "##addonpolicyretry" then retryRect = { ui.GetItemRect() } end
  if id == "##close" then closeSeen = true end -- (the gamepad pack's own close button)
  return hit
end
)lua") == LUA_OK);
            s.frames(2);
            CHECK(s.api.replaces("AddonPolicy")); // the client's own window stays off
            CHECK(window() == nullptr);           // nothing failed: nothing shown

            view.failed = view.can_retry = true;
            view.serial = 1;
            view.message = "Required add-on owner/thing failed with an error in a command";
            s.frames(3);
            ImGuiWindow* shown = window();
            REQUIRE(shown != nullptr);
            lua_getglobal(L, "closeSeen");
            CHECK((shown->HasCloseButton || lua_toboolean(L, -1))); // the player can put it away
            lua_settop(L, 0);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
            lua_getglobal(L, "retryRect");
            REQUIRE(lua_istable(L, -1));
            float rect[4]{};
            for (int i = 0; i < 4; ++i) {
                lua_rawgeti(L, -1, i + 1);
                rect[i] = static_cast<float>(lua_tonumber(L, -1));
                lua_pop(L, 1);
            }
            lua_settop(L, 0);
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent((rect[0] + rect[2]) / 2, (rect[1] + rect[3]) / 2);
            s.frames(1);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            io.AddMouseButtonEvent(0, false);
            s.frames(1);
            CHECK(retries == 1);

            // A failure Retry cannot help with: shown, without Retry.
            view.can_retry = false;
            view.serial = 2;
            luaL_dostring(L, "retryRect = nil");
            s.frames(3);
            CHECK(window() != nullptr);
            lua_getglobal(L, "retryRect");
            CHECK(lua_isnil(L, -1));
            lua_settop(L, 0);

            view.failed = false;
            view.serial = 3;
            s.frames(3);
            CHECK(window() == nullptr);
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

TEST_CASE("Modern Server Management uses authorized snapshots and submits only reviewed edits") {
    const fs::path source = fs::path(__FILE__).parent_path().parent_path() / "ui/modern";
    hailstone::test::ScratchDir scratch("ui_server_management_");
    const fs::path dir = scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(source, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    {
        Session session(dir, "modern");
        bool authorized = false;
        bool paused = false;
        int saves = 0;
        int reloads = 0;
        int full_snapshots = 0;
        int catalog_refreshes = 0;
        app::ServerManagementView snapshot;
        snapshot.serial = 1;
        snapshot.catalog_serial = 1;
        snapshot.revision = "42";
        snapshot.file_revision = "20261124T010000Z-zone-1";
        snapshot.minimum_status = 210;
        snapshot.fields = {{"commands", "block_groups", "movement_automation"},
                           {"commands", "gm", "/nav:200"},
                           {"addons", "required", "example/hud:1.0.0"}, {"local_extension", "keep_me", "original"},
                           {"commands", "default", "deny"}};
        snapshot.commands = {{"/nav", "Walk a route", "extended", {}, {"movement_automation", "non_rof2"}},
                             {"/moveto", "Walk to a location", "extended", {}, {"movement_automation", "non_rof2"}},
                             {"/finditem", "Search your inventory", "extended", {"/advi"}, {"non_rof2"}},
                             {"/follow", "Follow a player", "stock", {}, {}},
                             {"/target", "Select a target", "stock", {}, {}},
                             {"/camp", "Camp", "stock", {}, {}},
                             {"/quit", "Quit", "stock", {}, {}},
                             {"/exit", "Exit", "stock", {}, {}},
                             {"/servermanagement", "Open this window", "extended", {}, {"non_rof2"}},
                             {"/notes", "Add-on notes", "addon", {}, {"addon_commands"}}};
        session.host.server_management_available = [] { return true; };
        session.host.server_management = [&](std::uint32_t known, std::uint32_t known_catalog) -> std::optional<app::ServerManagementView> {
            if (!authorized) return std::nullopt;
            if (paused) return app::ServerManagementView{.paused = true};
            if (known != snapshot.serial) {
                ++full_snapshots;
                return snapshot;
            }
            app::ServerManagementView metadata;
            metadata.serial = snapshot.serial;
            metadata.catalog_serial = snapshot.catalog_serial;
            metadata.revision = snapshot.revision;
            metadata.file_revision = snapshot.file_revision;
            metadata.saving = snapshot.saving;
            metadata.reloading = snapshot.reloading;
            metadata.reload_required = snapshot.reload_required;
            metadata.message = snapshot.message;
            if (known_catalog != snapshot.catalog_serial) {
                ++catalog_refreshes;
                metadata.commands = snapshot.commands;
            }
            return metadata;
        };
        session.host.open_server_management = [&] {
            CHECK(snapshot.reload_required);
            CHECK_FALSE(snapshot.reloading);
            snapshot.reloading = true;
            ++reloads;
            return true;
        };
        session.host.save_server_management = [&](std::string_view revision, const auto& edits) {
            ++saves;
            CHECK(revision == (saves == 1 ? "43" : saves == 2 ? "44" : "45"));
            REQUIRE(edits.size() == (saves <= 2 ? 2 : 1));
            if (saves <= 2) {
                CHECK(edits.front().section == "commands");
                CHECK(edits.front().key == "blocked_message");
                CHECK(edits.front().value == "This command was blocked by your server admin.");
            }
            CHECK(edits.back().section == "commands");
            CHECK(edits.back().key == "gm");
            CHECK(edits.back().value == "/nav:253, /targ:205");
            snapshot.saving = true;
            return true;
        };
        auto visible = [] {
            ImGuiWindow* win = ImGui::FindWindowByName("Server Management###modern.ServerManagement");
            return win && win->WasActive;
        };
        lua_State* L = session.lua.state();
        REQUIRE(luaL_dostring(L, R"lua(
local draftModel = dofile("lib/server_management.lua")
local draft = draftModel.new({
  fields = {
    { section = "commands", key = "allow", value = "Ephemeral, /ADVI, :, /" },
    { section = "commands", key = "block_groups", value = "non_rof2" },
    { section = "commands", key = "gm", value = "/Retained:typo" },
    { section = "addons", key = "allow", value = "https://github.com/example/hud, example/keep" },
    { section = "addons", key = "required", value = "example/hud : V1.2.0-rc.10+written, example/hud:1.2.0-rc.2, " ..
      "example/hud:1.2.0-rc.10+equal, example/release:2.0.0+release, example/release:2.0.0-rc.1, " ..
      "example/text:3.1.0-alpha.zz, example/text:3.1.0-alpha.100, example/text:3.1.0-alpha" },
    { section = "addons", key = "suggest", value = "example/other@v1.3.0" }
  },
  commands = {
    { name = "/finditem", description = "Search inventory", origin = "extended", aliases = { "/advi" }, groups = { "non_rof2" } },
    { name = "/emote", description = "Emote", origin = "stock", aliases = { ":" }, groups = {} }
  }
})
local command = assert(draftModel.filterCommands(draft, "extended", "AdVi")[1])
assert(draftModel.commandEffective(draftModel.commandRules(draft), command) == "allow")
assert(draftModel.commandRules(draft).commands["/emote"] == "allow")
draftModel.setCommandRule(draft, "/advi", "", false)
assert(draftModel.commandEffective(draftModel.commandRules(draft), command) == "block")
assert(draftModel.get(draft, "commands.allow") == "Ephemeral, :, /")
draftModel.set(draft, "commands.gm", "/ADVI:200, /finditem:150, ::210, /Unlisted:75, /Retained:typo")
local rule, _, status = draftModel.commandEffective(draftModel.commandRules(draft), command)
assert(rule == "gm" and status == 200)
assert(draftModel.commandRules(draft).gm["/emote"].minimum == 210)
draftModel.setCommandRule(draft, "/advi", "gm", false, 253)
local retained = draftModel.get(draft, "commands.gm")
assert(retained == "/ADVI:253, ::210, /Unlisted:75, /Retained:typo")
assert(draftModel.validCommandGmList(retained, draft.saved["commands.gm"]))
assert(not draftModel.validCommandGmList(retained .. ", /Retained:typo", draft.saved["commands.gm"]))
assert(not draftModel.validCommandGmList(retained .. ", /Other:typo", draft.saved["commands.gm"]))
draftModel.set(draft, "commands.block", "/ADVI")
assert(draftModel.commandEffective(draftModel.commandRules(draft), command) == "block")
draftModel.set(draft, "commands.block", "")
draftModel.set(draft, "commands.gm", "/ADVI:oops, /finditem:150")
rule, _, status = draftModel.commandEffective(draftModel.commandRules(draft), command)
assert(rule == "gm" and status == nil)
draftModel.setCommandRule(draft, "/advi", "gm")
assert(draftModel.get(draft, "commands.gm") == "/ADVI:200")
draftModel.setCommandRule(draft, "/advi", "allow")
assert(draftModel.get(draft, "commands.gm") == "")
assert(draftModel.commandEffective(draftModel.commandRules(draft), command) == "allow")
assert(not draftModel.validCommandGmList("/finditem:0"))
assert(not draftModel.validCommandGmList("/finditem:32768"))
assert(not draftModel.validCommandGmList("/:200"))
assert(draftModel.validCommandGmList("/finditem:32767, ::150"))
local _, byId = draftModel.addons(draft)
local addon = assert(byId["example/hud"])
assert(addon.mode == "required" and addon.minimum == "V1.2.0-rc.10+written")
assert(byId["example/release"].minimum == "2.0.0+release")
assert(byId["example/text"].minimum == "3.1.0-alpha.zz")
assert(draft.edits["addons.required"] == nil)
draftModel.setAddonMinimum(draft, addon, "2.0.1")
assert(draftModel.get(draft, "addons.required") == "example/hud:2.0.1, example/release:2.0.0+release, " ..
  "example/release:2.0.0-rc.1, example/text:3.1.0-alpha.zz, example/text:3.1.0-alpha.100, example/text:3.1.0-alpha")
assert(draftModel.get(draft, "addons.allow") == "https://github.com/example/hud, example/keep")
assert(draftModel.get(draft, "addons.suggest") == "example/other@v1.3.0")

local target = { name = "/target", description = "Select a target", origin = "stock", aliases = {}, groups = {} }
local resolves = 0
local prefixDraft = draftModel.new({ fields = {}, commands = { target } }, function(name)
  assert(name == "/targ")
  resolves = resolves + 1
  return "/target"
end)
draftModel.set(prefixDraft, "commands.gm", "/TARG:204")
rule, _, status = draftModel.commandEffective(draftModel.commandRules(prefixDraft), target)
assert(rule == "gm" and status == 204)
draftModel.setCommandRule(prefixDraft, "/target", "gm", false, 205)
assert(draftModel.get(prefixDraft, "commands.gm") == "/TARG:205" and resolves == 1)

local button, input, select, color, invisibleButton, header =
  ui.Button, ui.InputTextWithHint, ui.Selectable, ui.TextColored, ui.InvisibleButton, ui.CollapsingHeader
ui.Button = function(label, ...)
  local clicked = button(label, ...)
  if clickManagement == label then clickManagement = nil return true end
  return clicked
end
ui.InputTextWithHint = function(label, ...)
  local value, changed = input(label, ...)
  if label == "##commands.blocked_message" and managementText then
    value, changed, managementText = managementText, true, nil
  elseif label == "##smgm/nav" and managementStatus then
    value, changed, managementStatus = managementStatus, true, nil
  elseif label == "##commands.gm" and managementGmList then
    value, changed, managementGmList = managementGmList, true, nil
  elseif label == "##smgm/target" and managementTargetStatus then
    value, changed, managementTargetStatus = managementTargetStatus, true, nil
  end
  return value, changed
end
ui.CollapsingHeader = function(label, ...)
  local open = header(label, ...)
  return open or (label == "Command lists" and managementGmList ~= nil)
end
ui.Selectable = function(label, ...)
  local clicked = select(label, ...)
  if label == visitManagement then visitManagement = nil return true end
  return clicked
end
ui.InvisibleButton = function(label, ...)
  local clicked = invisibleButton(label, ...)
  if label == originManagement then originManagement = nil return true end
  return clicked
end
ui.TextColored = function(r, g, b, a, text)
  if text == "GM access / 210+" then managementRankSeen = true end
  if text == "Blocked" then managementBlockSeen = true end
  if text == "GM 200+" then managementInitialRuleSeen = true end
  if text == "GM 253+" then managementUpdatedRuleSeen = true end
  if text == "GM 204+" then managementPrefixSeen = true end
  if text == "GM 205+" then managementCanonicalSeen = true end
  if text == "Review changes" then managementReviewSeen = true end
  if text == "This policy restricts /camp, /quit, /exit, /servermanagement. Review these rules before saving." then
    if managementReviewSeen then managementReviewWarningSeen = true else managementWarningSeen = true end
  end
  if text == "GM rules require command:status with a status from 1 to 32767." then managementValidationSeen = true end
  if text == "/notes" then managementNotesSeen = true end
  if text == "/freshaddon" then managementFreshAddonSeen = true end
  if text == "The editor snapshot expired. Reload before saving." then managementExpiredSeen = true end
  if text == "Reloading policy..." then managementReloadingSeen = true end
  if text == "/nav:195 -> /nav:253, /targ:205" then managementServerValueSeen = true end
  if text == "Resolve the changed server values before saving." then managementConflictSeen = true end
  color(r, g, b, a, text)
end
)lua") == LUA_OK);
        session.frames(2);
        CHECK_FALSE(visible());
        authorized = true;
        session.frames(3);
        CHECK(visible());
        CHECK(full_snapshots == 1);
        REQUIRE(luaL_dostring(L, "assert(managementRankSeen and managementBlockSeen and managementInitialRuleSeen and managementWarningSeen)") == LUA_OK);
        // Exercise all controls' immediate-mode window paths, including the add-on list and smaller viewport.
        for (const char* label : {"Client & gameplay", "Add-ons", "Scripts & plugins", "Socials", "Assets & web", "Commands"}) {
            lua_pushstring(L, label);
            lua_setglobal(L, "visitManagement");
            session.frames(2);
            REQUIRE(luaL_dostring(L, "assert(visitManagement == nil)") == LUA_OK);
        }
        session.display_size = {800, 560};
        session.frames(2);
        REQUIRE(luaL_dostring(L, R"lua(
managementStatus = "0"
clickManagement = "Review changes##sm"
)lua") == LUA_OK);
        session.frames(2);
        REQUIRE(luaL_dostring(L, "assert(managementValidationSeen and not managementReviewSeen)") == LUA_OK);
        REQUIRE(luaL_dostring(L, R"lua(
managementStatus = "253"
managementText = "This command was blocked by your server admin."
)lua") == LUA_OK);
        session.frames(2);
        REQUIRE(luaL_dostring(L, R"lua(
originManagement = '##smorigin1'
managementGmList = "/nav:253, /targ:204"
)lua") == LUA_OK);
        session.frames(2);
        REQUIRE(luaL_dostring(L, "assert(managementPrefixSeen); managementTargetStatus = '205'") == LUA_OK);
        session.frames(2);
        REQUIRE(luaL_dostring(L, "assert(managementCanonicalSeen)") == LUA_OK);
        REQUIRE(luaL_dostring(L, "originManagement = '##smorigin3'") == LUA_OK);
        session.frames(2);
        REQUIRE(luaL_dostring(L, "assert(managementUpdatedRuleSeen and managementNotesSeen and not managementFreshAddonSeen)") == LUA_OK);
        snapshot.catalog_serial = 2;
        snapshot.commands.back() = {"/freshaddon", "Newly registered command", "addon", {}, {"addon_commands"}};
        REQUIRE(luaL_dostring(L, "managementNotesSeen = nil") == LUA_OK);
        session.frames(2);
        CHECK(full_snapshots == 1);
        CHECK(catalog_refreshes == 1);
        REQUIRE(luaL_dostring(L, "assert(managementFreshAddonSeen and not managementNotesSeen)") == LUA_OK);
        REQUIRE(luaL_dostring(L, R"lua(
clickManagement = "Review changes##sm"
)lua") == LUA_OK);
        session.frames(2);
        REQUIRE(luaL_dostring(L, "assert(managementReviewWarningSeen)") == LUA_OK);
        paused = true;
        session.host.in_game = false;
        session.frames(2);
        CHECK_FALSE(visible());
        paused = false;
        session.host.in_game = true;
        snapshot.serial = 2;
        snapshot.revision = "43";
        snapshot.reloaded = true;
        session.frames(2);
        CHECK(visible());
        CHECK(full_snapshots == 2);
        REQUIRE(luaL_dostring(L, "clickManagement = 'Save policy##sm'") == LUA_OK);
        session.frames(2);
        CHECK(saves == 1);
        snapshot.saving = false;
        snapshot.reload_required = true;
        snapshot.message = "The editor snapshot expired. Reload before saving.";
        REQUIRE(luaL_dostring(L, "clickManagement = 'Save policy##sm'") == LUA_OK);
        session.frames(2);
        CHECK(saves == 1);
        REQUIRE(luaL_dostring(L, "assert(managementExpiredSeen); clickManagement = 'Reload policy##sm'") == LUA_OK);
        session.frames(2);
        CHECK(reloads == 1);
        REQUIRE(luaL_dostring(L, "assert(managementReloadingSeen)") == LUA_OK);
        snapshot.serial = 3;
        snapshot.revision = "44";
        snapshot.reload_required = false;
        snapshot.reloading = false;
        snapshot.reloaded = true;
        snapshot.message.clear();
        session.frames(2);
        CHECK(full_snapshots == 3);
        REQUIRE(luaL_dostring(L, "clickManagement = 'Save policy##sm'") == LUA_OK);
        session.frames(2);
        CHECK(saves == 2);

        // A lost confirmation may mean a change already landed. If a different value arrived for another key,
        // compare it with the proposed edit and require a choice before allowing the new save.
        snapshot.saving = false;
        snapshot.reload_required = true;
        REQUIRE(luaL_dostring(L, "clickManagement = 'Reload policy##sm'") == LUA_OK);
        session.frames(2);
        CHECK(reloads == 2);
        snapshot.serial = 4;
        snapshot.revision = "45";
        snapshot.reload_required = false;
        snapshot.reloading = false;
        snapshot.fields.push_back({"commands", "blocked_message", "This command was blocked by your server admin."});
        snapshot.fields[1].value = "/nav:195";
        session.frames(2);
        CHECK(full_snapshots == 4);
        REQUIRE(luaL_dostring(L, R"lua(
assert(managementConflictSeen and managementServerValueSeen)
clickManagement = 'Save policy##sm'
)lua") == LUA_OK);
        session.frames(2);
        CHECK(saves == 2);
        REQUIRE(luaL_dostring(L, "clickManagement = 'Keep my edit##commands.gm'") == LUA_OK);
        session.frames(2);
        CHECK(saves == 2);
        REQUIRE(luaL_dostring(L, "clickManagement = 'Save policy##sm'") == LUA_OK);
        session.frames(2);
        CHECK(saves == 3);

        snapshot.serial = 5;
        snapshot.saving = false;
        snapshot.reloaded = false;
        snapshot.revision = "46";
        snapshot.fields[1].value = "/nav:253, /targ:205";
        session.frames(2);
        CHECK(full_snapshots == 5);
        session.host.trusted_ui = false;
        session.frames(2);
        CHECK_FALSE(visible());
        REQUIRE(luaL_dostring(L, R"lua(
assert(not hs.UI.serverManagement.Available())
assert(not hs.UI.serverManagement.Save("46", {}))
)lua") == LUA_OK);
        CHECK(saves == 3);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Modern Server Management edits TLO policy and keeps the draft during catalog changes and reloads") {
    const fs::path source = fs::path(__FILE__).parent_path().parent_path() / "ui/modern";
    hailstone::test::ScratchDir scratch("ui_server_tlo_policy_");
    const fs::path dir = scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(source, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    {
        Session session(dir, "modern");
        app::ServerManagementView snapshot;
        snapshot.serial = snapshot.catalog_serial = 1;
        snapshot.revision = "42";
        snapshot.fields = {{"tlos", "allow", "mE, Unloaded"}, {"tlos", "block", "sPaWn, Me, Retained"},
                           {"local_extension", "keep_me", "original"}};
        snapshot.tlos = {{"Me", "Your character", "builtin"}, {"Spawn", "Search nearby spawns", "builtin"},
                         {"RaidNotes", "Live notes", "addon"}, {"Odd.Name", "Name outside the policy format", "addon"}};
        session.host.server_management = [&](std::uint32_t known, std::uint32_t known_catalog)
            -> std::optional<app::ServerManagementView> {
            auto view = snapshot;
            if (known == view.serial) {
                view.fields.clear();
                if (known_catalog == view.catalog_serial) view.tlos.clear();
            }
            return view;
        };
        int reloads = 0;
        session.host.open_server_management = [&] {
            CHECK(snapshot.reload_required);
            snapshot.reloading = true;
            ++reloads;
            return true;
        };
        int saves = 0;
        session.host.save_server_management = [&](std::string_view revision, const auto& edits) {
            ++saves;
            CHECK(revision == "43");
            REQUIRE(edits.size() == 3);
            for (const auto& edit : edits) CHECK(edit.section == "tlos");
            CHECK(edits[0].key == "allow");
            CHECK(edits[0].value == "mE, Unloaded, sPaWn");
            CHECK(edits[1].key == "block");
            CHECK(edits[1].value == "Retained, futureTLO");
            CHECK(edits[2].key == "default");
            CHECK(edits[2].value == "deny");
            return true;
        };
        lua_State* L = session.lua.state();
        auto lua = [&](const char* body) {
            const int result = luaL_dostring(L, body);
            REQUIRE_MESSAGE(result == LUA_OK, (result == LUA_OK ? "" : lua_tostring(L, -1)));
        };
        lua(R"lua(
local begin, finish, select, input, color, button, tab, header =
    ui.BeginCombo, ui.EndCombo, ui.Selectable, ui.InputTextWithHint, ui.TextColored, ui.Button,
    ui.InvisibleButton, ui.CollapsingHeader
local faked, current
tloSeen, tloEffective = {}, {}
ui.BeginCombo = function(id, ...)
  if id == "##smtloruleodd.name" then invalidTloRule = true end
  if tloCombo and id == tloCombo then faked = true return true end
  return begin(id, ...)
end
ui.EndCombo = function()
  if faked then faked = false return end
  finish()
end
ui.Selectable = function(label, ...)
  if faked then
    if label == tloPick then tloPick, tloCombo = nil, nil return true end
    return false
  end
  local clicked = select(label, ...)
  if label == tloPage then tloPage = nil return true end
  return clicked
end
ui.InputTextWithHint = function(id, ...)
  if id == "##smtlolisearch" and tloSearch ~= nil then
    local text = tloSearch tloSearch = nil return text, true
  end
  if id == "##tlos.block" and tloBlock ~= nil then
    local text = tloBlock tloBlock = nil return text, true
  end
  return input(id, ...)
end
ui.CollapsingHeader = function(label, ...)
  return header(label, ...) or (label == "TLO lists" and tloBlock ~= nil)
end
ui.TextColored = function(r, g, b, a, label)
  if label == "Me" or label == "Spawn" or label == "Odd.Name" or label == "RaidNotes" or label == "RaidPlan"
      or label == "Unloaded" or label == "Retained" or label == "futureTLO" then
    tloSeen[label], current = true, label
  elseif current and (label == "Allowed" or label == "Blocked") then
    tloEffective[current], current = label, nil
  end
  if label == "Top-level objects" then tloHeading = true end
  if label == "Deny by default can break scripts that depend on TLOs such as Me, If, or Math." then
    if tloReviewSeen then tloReviewWarning = true else tloPageWarning = true end
  end
  if label == "Review changes" then tloReviewSeen = true end
  color(r, g, b, a, label)
end
ui.Button = function(label, ...)
  local clicked = button(label, ...)
  if label == tloAction then tloAction = nil return true end
  return clicked
end
ui.InvisibleButton = function(label, ...)
  local clicked = tab(label, ...)
  if label == tloTab then tloTab = nil return true end
  return clicked
end
tloPage = "TLO access"
)lua");
        session.frames(3);
        lua("assert(tloHeading and tloEffective.Me == 'Blocked' and tloEffective.Spawn == 'Blocked')");
        lua("tloSeen = {}; tloSearch = 'NEARBY SPAWNS'");
        session.frames(2);
        lua("assert(tloSeen.Spawn and not tloSeen.Me); tloSearch = ''; tloCombo, tloPick = '##tlos.default', 'Deny'");
        session.frames(2);
        lua("assert(tloPageWarning); tloCombo, tloPick = '##smtloruleme', 'Allow'");
        session.frames(2);
        lua("assert(tloEffective.Me == 'Allowed'); tloCombo, tloPick = '##smtlorulespawn', 'Allow'");
        session.frames(2);
        lua("assert(tloEffective.Spawn == 'Allowed'); tloTab = '##smtloorigin2'");
        session.frames(2);
        lua("assert(tloSeen.RaidNotes and tloSeen.Unloaded and tloSeen.Retained and tloSeen['Odd.Name']); "
            "assert(tloEffective.Unloaded == 'Allowed' and tloEffective['Odd.Name'] == 'Blocked' and not invalidTloRule); tloSeen = {}");
        ++snapshot.catalog_serial;
        snapshot.tlos[2] = {"RaidPlan", "Live plans", "addon"};
        session.frames(2);
        lua("assert(tloSeen.RaidPlan and tloSeen.Retained and not tloSeen.RaidNotes); tloBlock = 'Retained, futureTLO'");
        session.frames(2);
        lua("assert(tloEffective.futureTLO == 'Blocked'); tloAction = 'Review changes##sm'");
        session.frames(2);
        lua("assert(tloReviewSeen and tloReviewWarning)");
        CHECK(saves == 0);
        snapshot.reload_required = true;
        lua("tloAction = 'Reload policy##sm'");
        session.frames(2);
        CHECK(reloads == 1);
        ++snapshot.serial;
        snapshot.revision = "43";
        snapshot.reload_required = snapshot.reloading = false;
        snapshot.reloaded = true;
        session.frames(2);
        lua("tloAction = 'Save policy##sm'");
        session.frames(2);
        CHECK(saves == 1);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("Modern Server Management reviews add-on dependencies and writes only explicit TLO grants") {
    const fs::path source = fs::path(__FILE__).parent_path().parent_path() / "ui/modern";
    hailstone::test::ScratchDir scratch("ui_server_addon_tlo_grants_");
    const fs::path dir = scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(source, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    {
        Session session(dir, "modern");
        app::ServerManagementView view;
        view.serial = view.catalog_serial = 1;
        view.revision = "42";
        view.fields = {{"addons", "allow", "Team/RaidNotes, old/legacy"},
                       {"addons", "required", "other/needsme:1.2.0"},
                       {"addons", "suggest", "team/checks"},
                       {"tlos", "block", "Me, Spawn"},
                       {"tlos", "addon_allow", "Team/RaidNotes:Retired, other/unloaded:Me"}};
        session.host.server_management = [&](std::uint32_t, std::uint32_t) -> std::optional<app::ServerManagementView> {
            return view;
        };
        int lookups = 0;
        bool loaded = false;
        session.host.server_management_addon_tlos =
            [&](std::string_view addon, bool request) -> std::optional<app::ServerManagementAddonTlos> {
            if (request) {
                CHECK(addon == "team/raidnotes");
                ++lookups;
            }
            if (addon == "team/raidnotes") {
                if (loaded) {
                    return app::ServerManagementAddonTlos{"team/raidnotes", "2.1.0", "ready",
                        std::vector<std::string>{"Me", "Target"}, std::vector<std::string>{"Spawn", "mE"}};
                }
                return app::ServerManagementAddonTlos{"team/raidnotes", "", lookups ? "pending" : "unchecked", {}, {}};
            }
            if (addon == "other/needsme") {
                return app::ServerManagementAddonTlos{"other/needsme", "1.2.0", "ready",
                    std::vector<std::string>{"Me"}, std::vector<std::string>{}};
            }
            if (addon == "team/checks") {
                return app::ServerManagementAddonTlos{"team/checks", "1.2.0", "ready",
                    std::vector<std::string>{"Spawn"}, std::vector<std::string>{}};
            }
            return std::nullopt;
        };
        int saves = 0;
        session.host.save_server_management = [&](std::string_view revision, const auto& edits) {
            ++saves;
            CHECK(revision == "42");
            REQUIRE(edits.size() == 1);
            CHECK(edits[0].section == "tlos");
            CHECK(edits[0].key == "addon_allow");
            CHECK(edits[0].value == "other/unloaded:Me, team/raidnotes:Me, team/raidnotes:Spawn");
            return true;
        };
        lua_State* L = session.lua.state();
        auto lua = [&](const char* body) {
            const int result = luaL_dostring(L, body);
            REQUIRE_MESSAGE(result == LUA_OK, (result == LUA_OK ? "" : lua_tostring(L, -1)));
        };
        lua(R"lua(
local select, button, colored = ui.Selectable, ui.Button, ui.TextColored
local current
labels, access, dependency = {}, {}, {}
ui.Selectable = function(label, ...)
  local clicked = select(label, ...)
  if label == pageChoice then pageChoice = nil return true end
  return clicked
end
ui.Button = function(label, ...)
  local clicked = button(label, ...)
  if label == actionChoice then actionChoice = nil return true end
  return clicked
end
ui.TextColored = function(r, g, b, a, label)
  labels[label] = true
  if label == "Me" or label == "Spawn" or label == "Target" or label == "Retired" then
    current = label
  elseif current and (label == "Required" or label == "Suggested" or label == "Existing grant") then
    dependency[current] = label
  elseif current and (label == "Conflict" or label == "Denied" or label == "Allowed" or label == "Granted") then
    access[current], current = label, nil
  end
  colored(r, g, b, a, label)
end
pageChoice = "Add-ons"
)lua");
        session.frames(3);
        lua("actionChoice = 'Review TLOs##smaddonteam/raidnotes'");
        session.frames(2);
        lua("assert(labels['TLO dependencies'] and labels['Required TLOs are unknown.']); "
            "actionChoice = 'Check dependencies##smaddontlo'");
        session.frames(2);
        CHECK(lookups == 1);
        loaded = true;
        session.frames(2);
        lua("assert(labels['Version 2.1.0'] and access.Me == 'Conflict' and access.Spawn == 'Denied' and "
            "access.Target == 'Allowed' and access.Retired == 'Granted'); "
            "assert(dependency.Me == 'Required' and dependency.Spawn == 'Suggested' and "
            "dependency.Retired == 'Existing grant'); actionChoice = 'Grant##smaddontlome'");
        session.frames(2);
        lua("assert(access.Me == 'Granted'); actionChoice = 'Grant##smaddontlospawn'");
        session.frames(2);
        lua("assert(access.Spawn == 'Granted'); actionChoice = 'Revoke##smaddontloretired'");
        session.frames(2);
        lua("actionChoice = 'Review changes##sm'");
        session.frames(2);
        lua("assert(labels['other/needsme requires denied TLOs: Me'] and labels['team/checks requires denied TLOs: Spawn'] "
            "and labels['Required TLOs are unknown for 1 add-on. Check dependencies on the Add-ons page.']); "
            "assert(not labels['team/raidnotes requires denied TLOs: Me']); actionChoice = 'Save policy##sm'");
        session.frames(2);
        CHECK(lookups == 1);
        CHECK(saves == 1);
        session.host.trusted_ui = false;
        lua("assert(hs.UI.serverManagement.AddonTlos('team/raidnotes', true) == nil)");
        CHECK(lookups == 1);
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// A click meant for something else the instant the question pops up must not answer it: both buttons wait a
// second after the question shows. The real loading cover must leave both answers reachable afterward.
TEST_CASE("UI packs: both plugin answers arm and remain reachable above the loading cover") {
    for (const bool in_game : {true, false})
    for (const bool allow : {true, false})
    for (const char* pack : {"modern", "gamepad"}) {
        CAPTURE(pack);
        CAPTURE(in_game);
        CAPTURE(allow);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch(std::string("ui_pack_consent_arm_") + pack +
                                               (in_game ? "_game" : "_load") + (allow ? "_allow" : "_deny"));
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            Session s(dir, pack, {}, in_game);
            ui::LoadingView loading;
            loading.title = "Checking required add-ons";
            loading.detail = "Review the required plugin's permission request to continue.";
            std::optional<plugin::ConsentPrompt> prompt;
            std::vector<std::optional<bool>> answers;
            s.host.plugin_consent = [&] { return prompt; };
            s.host.answer_plugin_consent = [&](std::optional<bool> a) {
                answers.push_back(a);
                prompt.reset();
            };
            lua_State* L = s.lua.state();
            REQUIRE(luaL_dostring(L, R"lua(
local button, invisible = ui.Button, ui.InvisibleButton
ui.Button = function(label, ...)
  local hit = button(label, ...)
  if label == "Allow##consent" then allowRect = { ui.GetItemRect() } end
  if label == "Don't allow##consent" then denyRect = { ui.GetItemRect() } end
  return hit
end
ui.InvisibleButton = function(id, ...)
  local hit = invisible(id, ...)
  if id == "##consentallow" then allowRect = { ui.GetItemRect() } end
  if id == "##consentdeny" then denyRect = { ui.GetItemRect() } end
  return hit
end
)lua") == LUA_OK);
            auto click_answer = [&] {
                lua_getglobal(L, allow ? "allowRect" : "denyRect");
                REQUIRE(lua_istable(L, -1));
                float rect[4]{};
                for (int i = 0; i < 4; ++i) {
                    lua_rawgeti(L, -1, i + 1);
                    rect[i] = static_cast<float>(lua_tonumber(L, -1));
                    lua_pop(L, 1);
                }
                lua_settop(L, 0);
                auto& io = ImGui::GetIO();
                io.AddMousePosEvent((rect[0] + rect[2]) / 2, (rect[1] + rect[3]) / 2);
                s.frames(1, nullptr, &loading);
                io.AddMouseButtonEvent(0, true);
                s.frames(1, nullptr, &loading);
                io.AddMouseButtonEvent(0, false);
                s.frames(1, nullptr, &loading);
            };
            plugin::ConsentPrompt p;
            p.server = "Alpha";
            p.serial = 4;
            p.files.push_back({"MQ2Thing", "MQ2Thing.so", 4096, {}});
            prompt = p;
            s.frames(3, nullptr, &loading);
            click_answer(); // within its first second: nothing
            CHECK(answers.empty());
            CHECK(prompt.has_value());
            s.frames(65, nullptr, &loading);
            click_answer();
            REQUIRE(answers.size() == 1);
            CHECK(answers[0] == std::optional<bool>(allow));
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
        }
        ImGui::DestroyContext(ctx);
    }
}

// The gamepad pack has no popup of its own: the client's prompt stays, and its group window gives it the pad.
TEST_CASE("UI packs (gamepad): an invitation to a group keeps the client's own prompt") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "gamepad";
    hailstone::test::ScratchDir dir_scratch("ui_pack_group_invite_gamepad_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "gamepad", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "gamepad");
        net::GroupInvite inv;
        inv.invitee = "Tester";
        inv.inviter = "Bob";
        s.conn.receive_zone_packet_for_test(net::encode(inv));
        s.frames(2);
        CHECK_FALSE(s.api.replaces("GroupInvite"));
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// A resizable dialog's bottom-right corner resizes it (the AA window): its size in design pixels grows on both
// axes and its scale stays (the scale is the right-click menu's). Its right edge changes the width alone.
TEST_CASE("UI packs (modern): a dialog's corner and edge resize it, neither scales it") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_dialog_corner_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern");
        lua_State* L = s.lua.state();
        auto setting = [&](const char* name) {
            REQUIRE(luaL_dostring(L, (std::string("return ui.GetSetting('") + name + "') or ''").c_str()) == LUA_OK);
            std::string v = lua_tostring(L, -1);
            lua_settop(L, 0);
            return v;
        };
        REQUIRE(luaL_dostring(L, "ui.SetSetting('hud.size.AAWindow', '600,450') ui.SetSetting('test.pos.AAWindow', "
                                 "'40,40') hs.ShowWindow('AAWindow', true)") == LUA_OK);
        s.frames(3);
        ImGuiWindow* win = ImGui::FindWindowByName("Alternate Advancement###modern.AAWindow");
        REQUIRE(win);
        const ImVec2 before = win->Size;
        ImGuiIO& io = ImGui::GetIO();
        auto drag = [&](ImVec2 from, float dx, float dy) {
            io.AddMousePosEvent(from.x, from.y);
            s.frames(1);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            for (int i = 1; i <= 10; ++i) {
                io.AddMousePosEvent(from.x + dx * static_cast<float>(i) / 10, from.y + dy * static_cast<float>(i) / 10);
                s.frames(1);
            }
            io.AddMouseButtonEvent(0, false);
            s.frames(1);
            io.AddMousePosEvent(-1000, -1000);
            s.frames(3);
        };
        // the corner: a larger design size on both axes, the scale untouched
        drag(ImVec2(win->Pos.x + win->Size.x - 4, win->Pos.y + win->Size.y - 4), 120, 90);
        CHECK(setting("hud.scale.AAWindow").empty());
        int sw = 0, sh = 0;
        REQUIRE(std::sscanf(setting("hud.size.AAWindow").c_str(), "%d,%d", &sw, &sh) == 2);
        CHECK(sw > 680);
        CHECK(sh > 510);
        CHECK(win->Size.x > before.x + 80);
        CHECK(win->Size.y > before.y + 60);
        // the right edge: a wider design size, the height and scale unchanged
        const ImVec2 grown = win->Size;
        drag(ImVec2(win->Pos.x + win->Size.x - 2, win->Pos.y + win->Size.y / 2), 60, 0);
        CHECK(setting("hud.scale.AAWindow").empty());
        CHECK(win->Size.x > grown.x + 30);
        CHECK(win->Size.y == doctest::Approx(grown.y).epsilon(0.01));
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("UI packs (modern): fallback chat appears only without a chat replacement") {
    namespace tabs = app::chat_tabs;
    const auto src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir scratch("ui_pack_fallback_chat");
    const auto dir = scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> ctx(ImGui::CreateContext(), ImGui::DestroyContext);
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    Session s(dir, "modern");
    s.lua.set_entry_scripts({"chat.lua"});
    ReplacementCheck replacements(s.api);
    ui::InGameView fallback;
    const auto fallback_active = [] {
        const ImGuiWindow* window = ImGui::FindWindowByName("Chat##hailstone");
        return window && window->Active;
    };

    s.frames(4, &fallback);
    REQUIRE(s.api.replaces("Chat1"));
    CHECK_FALSE(fallback_active());

    const int destination = tabs::new_window("Other Chat");
    REQUIRE(destination > 1);
    REQUIRE(tabs::combine(destination) > 0);
    REQUIRE(tabs::group(1).tabs.empty());
    const std::string slot = std::to_string(destination);
    const std::string screen = "Chat" + slot;
    s.frames(4, &fallback);
    CHECK_FALSE(s.api.replaces("Chat1"));
    REQUIRE(s.api.replaces(screen));
    const ImGuiWindow* modern = ImGui::FindWindowByName(("###modern." + screen).c_str());
    REQUIRE(modern);
    CHECK(modern->Active);
    CHECK_FALSE(fallback_active());

    s.host.keep_cpp_windows = true;
    s.frames(1, &fallback);
    CHECK(modern->Active);
    CHECK(fallback_active());
    s.host.keep_cpp_windows = false;

    const std::string disable = "ui.DisableWindow('Chat " + slot + "')";
    REQUIRE(luaL_dostring(s.lua.state(), disable.c_str()) == LUA_OK);
    s.frames(4, &fallback);
    CHECK_FALSE(s.api.replaces(screen));
    CHECK_FALSE(modern->Active);
    CHECK(fallback_active());
    CHECK(ImGui::FindWindowByName("UI errors") == nullptr);

    REQUIRE(tabs::detach(1) == 1);
    s.frames(4, &fallback);
    CHECK(s.api.replaces("Chat1"));
    CHECK_FALSE(fallback_active());
}

// A resizable HUD frame's bottom-right corner resizes it (the chat): a larger design size, the scale untouched. A
// frame sized to what it shows (the player) has no corner grip: a drag there moves it and leaves its scale alone.
TEST_CASE("UI packs (modern): a frame's corner resizes it, never scales it") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_frame_corner_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern");
        lua_State* L = s.lua.state();
        auto setting = [&](const char* name) {
            REQUIRE(luaL_dostring(L, (std::string("return ui.GetSetting('") + name + "') or ''").c_str()) == LUA_OK);
            std::string v = lua_tostring(L, -1);
            lua_settop(L, 0);
            return v;
        };
        REQUIRE(luaL_dostring(L, "ui.SetSetting('hud.size.Chat1', '400,200') ui.SetSetting('test.pos.Chat1', '40,300') "
                                 "ui.SetSetting('test.pos.PlayerWindow', '600,40')") == LUA_OK);
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(-1000, -1000);
        s.frames(3);
        ImGuiWindow* chat = ImGui::FindWindowByName("###modern.Chat1");
        ImGuiWindow* player = ImGui::FindWindowByName("Player###modern.PlayerWindow");
        REQUIRE(chat);
        REQUIRE(player);
        auto drag = [&](ImVec2 from, float dx, float dy) {
            io.AddMousePosEvent(from.x, from.y);
            s.frames(1);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            for (int i = 1; i <= 10; ++i) {
                io.AddMousePosEvent(from.x + dx * static_cast<float>(i) / 10, from.y + dy * static_cast<float>(i) / 10);
                s.frames(1);
            }
            io.AddMouseButtonEvent(0, false);
            s.frames(1);
            io.AddMousePosEvent(-1000, -1000);
            s.frames(3);
        };
        const ImVec2 before = chat->Size;
        drag(ImVec2(chat->Pos.x + chat->Size.x - 4, chat->Pos.y + chat->Size.y - 4), 100, 60);
        CHECK(setting("hud.scale.Chat1").empty());
        int sw = 0, sh = 0;
        REQUIRE(std::sscanf(setting("hud.size.Chat1").c_str(), "%d,%d", &sw, &sh) == 2);
        CHECK(sw > 460);
        CHECK(sh > 230);
        CHECK(chat->Size.x > before.x + 60);
        CHECK(chat->Size.y > before.y + 30);
        const ImVec2 playerSize = player->Size;
        drag(ImVec2(player->Pos.x + player->Size.x - 4, player->Pos.y + player->Size.y - 4), 80, 40);
        CHECK(setting("hud.scale.PlayerWindow").empty());
        CHECK(player->Size.x == doctest::Approx(playerSize.x));
        CHECK(player->Size.y == doctest::Approx(playerSize.y));
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// A dialog's roll-up button sits beside its close box (ImGui's collapse button at the right of the title bar): a click
// rolls it up to its title bar and keeps that in hud.rolled.<window>, through closing and opening it again; a second
// click unrolls it. A question to answer (rollup = false) has none.
TEST_CASE("UI packs (modern): a dialog rolls up from the button beside its close box") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_dialog_rollup_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern");
        lua_State* L = s.lua.state();
        auto run = [&](const char* code) {
            REQUIRE(luaL_dostring(L, code) == LUA_OK);
            std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            lua_settop(L, 0);
            return v;
        };
        run("ui.SetSetting('test.pos.AAWindow', '40,40') hs.ShowWindow('AAWindow', true)");
        s.frames(3);
        ImGuiWindow* win = ImGui::FindWindowByName("Alternate Advancement###modern.AAWindow");
        REQUIRE(win);
        CHECK_FALSE(win->Collapsed);
        CHECK((win->Flags & ImGuiWindowFlags_NoCollapse) == 0);
        const float fullHeight = win->Size.y;
        ImGuiIO& io = ImGui::GetIO();
        auto clickRollup = [&] {
            // where ImGui puts it with WindowMenuButtonPosition = Right: left of the close box
            const ImGuiStyle& st = ImGui::GetStyle();
            const float fs = win->TitleBarHeight - st.FramePadding.y * 2;
            const ImVec2 at(win->Pos.x + win->Size.x - st.FramePadding.x - fs - st.ItemInnerSpacing.x - fs / 2,
                            win->Pos.y + st.FramePadding.y + fs / 2);
            io.AddMousePosEvent(at.x, at.y);
            s.frames(1);
            io.AddMouseButtonEvent(0, true);
            s.frames(1);
            io.AddMouseButtonEvent(0, false);
            s.frames(1);
            io.AddMousePosEvent(-1000, -1000);
            s.frames(2);
        };
        clickRollup();
        CHECK(win->Collapsed);
        CHECK(win->Size.y < fullHeight / 2);
        CHECK(run("return ui.GetSetting('hud.rolled.AAWindow')") == "1");
        CHECK(run("return tostring(hs.WindowShown('AAWindow'))") == "true"); // (rolled up, not closed)
        // closed and opened again: still rolled up
        run("hs.ShowWindow('AAWindow', false)");
        s.frames(2);
        run("hs.ShowWindow('AAWindow', true)");
        s.frames(2);
        CHECK(win->Active);
        CHECK(win->Collapsed);
        clickRollup();
        CHECK_FALSE(win->Collapsed);
        CHECK(run("return ui.GetSetting('hud.rolled.AAWindow') or ''") == "");
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// The gamepad pack's shared pieces that every window calls each frame, against the way they worked before they were
// made cheaper: T.fit (a binary search; never half a UTF-8 character), G.wrapped (a line measured whole only near the
// width; maxLines lines drawn, the bottom of all), CH.testOpen (off for good once test.show is empty in the world).
TEST_CASE("UI packs (gamepad): T.fit, G.wrapped and CH.testOpen give what they gave") {
    constexpr const char* kScript = R"lua(
local T = dofile("lib/theme.lua")
local G = dofile("lib/stock.lua")
local CH = dofile("lib/child.lua")
local function check(ok, what) if not ok then error(what, 2) end end
ui.Begin("gamepad lib test")
-- T.fit: the old byte-by-byte search, for ASCII
local function oldFit(text, w)
  if ui.CalcTextSize(text) <= w then return text end
  local n = #text
  while n > 1 and ui.CalcTextSize(text:sub(1, n) .. "...") > w do n = n - 1 end
  return text:sub(1, n) .. "..."
end
for _, text in ipairs({ "", "a", "ab", "Lord Nagafen's Lair", "A very long item name, of the ages (Legendary)" }) do
  for w = -5, 400, 3 do
    check(T.fit(text, w) == oldFit(text, w), "ascii " .. text .. " @" .. w)
  end
end
-- T.fit, UTF-8: the longest start that ends a character and fits, never fewer than the first character
local function isCont(b) return b and b >= 0x80 and b < 0xC0 end
for _, text in ipairs({ "\u{C9}l\u{E9}onore d\u{2019}Ambr\u{E9}e", "\u{1F600}\u{1F600}x\u{E9}\u{E9}\u{E9}", "\u{E9}" }) do
  local first = 1
  while isCont(text:byte(first + 1)) do first = first + 1 end
  for w = -5, 300, 2 do
    local got = T.fit(text, w)
    local want = text
    if ui.CalcTextSize(text) > w then
      local best = first
      for n = first, #text do
        if not isCont(text:byte(n + 1)) and ui.CalcTextSize(text:sub(1, n) .. "...") <= w then best = n end
      end
      want = text:sub(1, best) .. "..."
    end
    check(got == want, "utf8 @" .. w .. ": " .. got .. " / " .. want)
  end
end
-- G.wrapped: the old wrap (every line measured whole), the lines drawn and the bottom
local function oldWrapped(text, x, y, width, maxLines, out)
  local _, lh = ui.CalcTextSize("Ag")
  local n = 0
  for para in (text .. "\n"):gmatch("(.-)\n") do
    local line = ""
    for word in para:gmatch("%S+") do
      local try = line == "" and word or (line .. " " .. word)
      if ui.CalcTextSize(try) > width and line ~= "" then
        if not maxLines or n < maxLines then out[#out + 1] = y .. ":" .. line end
        y, n, line = y + lh, n + 1, word
      else
        line = try
      end
    end
    if not maxLines or n < maxLines then out[#out + 1] = y .. ":" .. line end
    y, n = y + lh, n + 1
  end
  return y
end
local words = {}
for i = 1, 120 do words[#words + 1] = ({ "a", "gnoll", "pup", "the", "Commonlands", "of", "W", "iiii", "MMMM" })[i % 9 + 1] end
local long = table.concat(words, " ") .. "\n\nA second paragraph  with  spaces.\nSupercalifragilisticexpialidocious!"
local drawn
local draw = T.draw
T.draw = function(x, y, text) drawn[#drawn + 1] = y .. ":" .. text end
for _, maxLines in ipairs({ false, 1, 3 }) do
  for width = 20, 600, 7 do
    drawn = {}
    local want = {}
    local yNew = G.wrapped(long, 10, 100, width, nil, maxLines or nil)
    local yOld = oldWrapped(long, 10, 100, width, maxLines or nil, want)
    check(yNew == yOld, "bottom @" .. width)
    check(table.concat(drawn, "|") == table.concat(want, "|"), "lines @" .. width)
    if maxLines then check(#drawn <= maxLines, "maxLines @" .. width) end
  end
end
T.draw = draw
ui.End()
-- CH.testOpen: test.show empty in the world turns it off for good (as M.testShow and G.testShow); a list is read
GPSTATE.testOpenOff = nil
ui.SetSetting("test.show", "NoSuchWnd,OtherWnd")
CH.testOpen("OtherWnd")
check(not GPSTATE.testOpenOff and GPSTATE.testOpenList.names.OtherWnd and GPSTATE.testOpenList.names.NoSuchWnd,
  "list read")
ui.SetSetting("test.show", "")
CH.testOpen("OtherWnd")
check(GPSTATE.testOpenOff == true, "off when empty")
ui.SetSetting("test.show", "OtherWnd")
GPSTATE.testOpenList = nil
CH.testOpen("OtherWnd")
check(GPSTATE.testOpenList == nil, "stays off")
return "ok"
)lua";
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "gamepad";
    hailstone::test::ScratchDir dir_scratch("ui_pack_gamepad_lib_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "gamepad", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "gamepad");
        s.frames(1);
        ImGui::GetIO().DisplaySize = ImVec2(1600, 900);
        s.api.begin_frame();
        ImGui::NewFrame();
        lua_State* L = s.lua.state();
        int rc = luaL_dostring(L, kScript);
        std::string out = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
        INFO(out);
        CHECK(rc == LUA_OK);
        CHECK(out == "ok");
        lua_settop(L, 0);
        ImGui::Render();
    }
    ImGui::DestroyContext(ctx);
}

namespace {
// Runs `script` in the gamepad pack (its lib/ files reachable with dofile) inside a window; it returns "ok".
std::string run_gamepad_lib_script(const char* script, const char* scratch_name) {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "gamepad";
    hailstone::test::ScratchDir dir_scratch(scratch_name);
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "gamepad", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    std::string out;
    {
        Session s(dir, "gamepad");
        s.frames(1);
        ImGui::GetIO().DisplaySize = ImVec2(1600, 900);
        s.api.begin_frame();
        ImGui::NewFrame();
        lua_State* L = s.lua.state();
        const int rc = luaL_dostring(L, script);
        out = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
        if (rc != LUA_OK) out = "error: " + out;
        lua_settop(L, 0);
        ImGui::Render();
    }
    ImGui::DestroyContext(ctx);
    return out;
}
} // namespace

// The gamepad pack's text edits (lib/theme.lua): Delete takes the last character, a length limit and a shortened
// title cut, and a text shortened from its start begins, only between UTF-8 characters; a byte that is not UTF-8
// (Latin-1) counts as one.
TEST_CASE("UI packs (gamepad): text edits never split a UTF-8 character") {
    constexpr const char* kScript = R"lua(
local T = dofile("lib/theme.lua")
local function check(ok, what) if not ok then error(what, 2) end end
-- Delete
check(T.dropChar("") == "", "empty")
check(T.dropChar("ab") == "a", "ascii")
check(T.dropChar("a\u{E9}") == "a", "two bytes")
check(T.dropChar("a\u{20AC}") == "a", "three bytes")
check(T.dropChar("a\u{1F600}") == "a", "four bytes")
check(T.dropChar("\u{E9}") == "", "only one")
check(T.dropChar("a\xA9") == "a", "latin-1 byte")
check(T.dropChar("\xA9\xA9") == "\xA9", "stray continuation bytes, one at a time")
-- a limit in bytes, cut between characters
check(T.cutBytes("abc", 5) == "abc" and T.cutBytes("abc", 2) == "ab" and T.cutBytes("abc", 0) == "", "ascii cut")
check(T.cutBytes("a\u{20AC}b", 1) == "a", "before")
check(T.cutBytes("a\u{20AC}b", 2) == "a", "inside (1)")
check(T.cutBytes("a\u{20AC}b", 3) == "a", "inside (2)")
check(T.cutBytes("a\u{20AC}b", 4) == "a\u{20AC}", "after")
check(T.cutBytes("\u{E9}\u{E9}", 3) == "\u{E9}", "two-byte")
check(T.cutBytes("ab\xA9\xA9\xA9\xA9c", 4) == "ab\xA9\xA9", "not UTF-8: by bytes")
-- every cut of a mixed text is a whole-character start of it
local text = "\u{C9}l\u{E9}onore \u{1F600} d\u{2019}Ambr\u{E9}e"
for n = 0, #text + 1 do
  local c = T.cutBytes(text, n)
  check(#c <= n and text:sub(1, #c) == c and utf8.len(c) ~= nil, "cut " .. n)
  check(n > #text or #c > n - 4, "cut too short " .. n)
end
-- a start moved on to a character
check(T.charStart("a\u{20AC}b", 1) == 1 and T.charStart("a\u{20AC}b", 2) == 2, "on a start")
check(T.charStart("a\u{20AC}b", 3) == 5 and T.charStart("a\u{20AC}b", 4) == 5, "inside")
check(T.charStart("a\u{E9}", 3) == 4, "to the end")
check(T.charStart("a\xA9\xA9\xA9\xA9", 2) == 2, "not UTF-8: as it is")
return "ok"
)lua";
    CHECK(run_gamepad_lib_script(kScript, "ui_pack_gamepad_utf8_test") == "ok");
}

// A click deferred until A is released (lib/window.lua W.deferred, used by lib/exchange.lua and lib/markets.lua)
// runs while the window it was asked in is still drawn, waits while A is held, and is dropped once that window is gone
// rather than firing into whichever window draws next.
TEST_CASE("UI packs (gamepad): a click deferred to A's release is dropped when its window closes") {
    constexpr const char* kScript = R"lua(
local W = dofile("lib/window.lua")
local function check(ok, what) if not ok then error(what, 2) end end
local held = false
local real = hs.UI.gamepad.Down
hs.UI.gamepad.Down = function(b) return b == "a" and held end
local frame = ui.GetFrameCount()
local ran = 0
local slot = {}
local function ask(owner)
  GPSTATE.currentWindow = owner
  W.deferred(slot, function() ran = ran + 1 end)
  GPSTATE.currentWindow = nil
end
-- its window drawn last frame: waits for A, then runs once
ask("Owner")
GPSTATE.drawn.Owner = frame - 1
held = true
W.runDeferred(slot)
check(ran == 0 and slot.fn ~= nil, "ran with A held")
held = false
W.runDeferred(slot)
check(ran == 1 and slot.fn == nil, "did not run on release")
W.runDeferred(slot)
check(ran == 1, "ran twice")
-- its window gone (not drawn for a few frames): dropped, even once A is up
ask("Owner")
GPSTATE.drawn.Owner = frame - 5
W.runDeferred(slot)
check(ran == 1 and slot.fn == nil, "a closed window's click fired")
-- asked outside a window: as before, runs on release
ask(nil)
W.runDeferred(slot)
check(ran == 2, "a click without a window did not run")
hs.UI.gamepad.Down = real
return "ok"
)lua";
    CHECK(run_gamepad_lib_script(kScript, "ui_pack_gamepad_deferred_test") == "ok");
}

// A pad button a window reads itself (T.settledPress: the Quantity, colour, appearance and icon dialogs' A) is
// dropped while the pack's settling time after a window gains the focus runs, as T.settle drops a button's press.
TEST_CASE("UI packs (gamepad): a dialog's own A waits out the settling time") {
    constexpr const char* kScript = R"lua(
local T = dofile("lib/theme.lua")
local function check(ok, what) if not ok then error(what, 2) end end
local down = true
local fake = { menuPressed = function(b) return b == "a" and down end }
GPSTATE.padSettleUntil = ui.GetTime() + 1
check(not T.settledPress(fake, "a"), "pressed while settling")
GPSTATE.padSettleUntil = ui.GetTime() - 1
check(T.settledPress(fake, "a"), "not pressed once settled")
down = false
check(not T.settledPress(fake, "a"), "pressed without a press")
GPSTATE.padSettleUntil = nil
down = true
check(T.settledPress(fake, "a"), "no settling time yet")
return "ok"
)lua";
    CHECK(run_gamepad_lib_script(kScript, "ui_pack_gamepad_settle_test") == "ok");
}

// One highlight (lib/theme.lua T.button, T.slot): with a pad given, its focus alone says which item is focused; the
// mouse resting over an item the pad's focus is not on does not light it as well. Without a pad, the mouse does. And
// the pack's mouse checks over a rectangle (T.mouseOver) need the window itself hovered: not a popup above it.
TEST_CASE("UI packs (gamepad): a pad's focus is the only highlight, and the mouse needs its window") {
    constexpr const char* kScript = R"lua(
local T = dofile("lib/theme.lua")
local function check(ok, what) if not ok then error(what, 2) end end
ui.Begin("gamepad highlight test")
local hovered, padFocus = ui.IsItemHovered, false
ui.IsItemHovered = function() return true end
local fake = { focused = function() return padFocus end, yActivated = function() return false end,
  navVisible = function() return false end }
local _, focused = T.button("##b1", "One", 100, 40, nil, fake)
check(focused == false, "button: the mouse lit an item the pad is not on")
_, focused = T.slot("##s1", nil, 40, nil, fake)
check(focused == false, "slot: the mouse lit an item the pad is not on")
padFocus = true
_, focused = T.button("##b2", "Two", 100, 40, nil, fake)
check(focused == true, "button: the pad's focus not shown")
_, focused = T.button("##b3", "Three", 100, 40)
check(focused == true, "button without a pad: the mouse not shown")
_, focused = T.slot("##s2", nil, 40)
check(focused == true, "slot without a pad: the mouse not shown")
ui.IsItemHovered = hovered
local overRect, overWindow = ui.IsMouseHoveringRect, ui.IsWindowHovered
local flags
ui.IsMouseHoveringRect = function() return true end
ui.IsWindowHovered = function(f) flags = f; return false end
check(not T.mouseOver(0, 0, 10, 10), "over a window above")
check(flags & ui.HoveredFlags.ChildWindows ~= 0 and flags & ui.HoveredFlags.AllowWhenBlockedByActiveItem ~= 0, "flags")
ui.IsWindowHovered = function() return true end
check(T.mouseOver(0, 0, 10, 10), "over its own window")
ui.IsMouseHoveringRect, ui.IsWindowHovered = overRect, overWindow
ui.End()
return "ok"
)lua";
    CHECK(run_gamepad_lib_script(kScript, "ui_pack_gamepad_highlight_test") == "ok");
}

// lib/markets.lua M.rows: a row that reads as nothing (gone since the count was read) is left out, so every later row
// is still there for ipairs and a sort, whether the whole list is read or a few rows a frame; filled again, it is back.
TEST_CASE("UI packs (gamepad): a market list's missing row leaves no hole") {
    constexpr const char* kScript = R"lua(
local M = dofile("lib/markets.lua")
local function check(ok, what) if not ok then error(what, 2) end end
local windows = hs.UI.windows
local info, row = windows.ListInfo, windows.ListRow
local LIST = {}
for i = 1, 30 do LIST[i] = "r" .. i end
local gone = { [3] = true }
windows.ListInfo = function() return #LIST, 1, 4 end
windows.ListRow = function(_, _, r)
  if gone[r + 1] then return nil end
  return { LIST[r + 1] }
end
local function names(rows)
  local out = {}
  for _, r in ipairs(rows) do out[#out + 1] = r.texts[1] end
  return out
end
-- read whole: 29 rows, the one after the hole kept
local rows, selected = M.rows("MarketTestWnd", "L")
local got = names(rows)
check(#got == 29 and #rows == 29 and got[3] == "r4" and got[29] == "r30", "whole: " .. table.concat(got, ","))
check(selected == 4, "selected")
-- filled again: read back in a few rows a frame (30 rows: 13 a call)
gone[3] = nil
for _ = 1, 3 do rows = M.rows("MarketTestWnd", "L") end
got = names(rows)
check(#got == 30 and got[3] == "r3", "filled: " .. table.concat(got, ","))
-- gone in the middle while read a few rows a frame
gone[10] = true
for _ = 1, 3 do rows = M.rows("MarketTestWnd", "L") end
got = names(rows)
check(#got == 29 and #rows == 29 and got[10] == "r11" and got[29] == "r30", "a few a frame: " .. table.concat(got, ","))
local copy = table.move(rows, 1, #rows, 1, {})
table.sort(copy, function(a, b) return a.index > b.index end) -- (a hole would hand the sort a nil)
windows.ListInfo, windows.ListRow = info, row
return "ok"
)lua";
    CHECK(run_gamepad_lib_script(kScript, "ui_pack_gamepad_market_rows_test") == "ok");
}

// The gamepad STML parser (lib/stml.lua): a "<" that opens no tag is text, shown with what follows it, in the colour
// it is in; a closed tag is still dropped.
TEST_CASE("UI packs (gamepad): STML keeps the text after an unmatched <") {
    constexpr const char* kScript = R"lua(
local S = dofile("lib/stml.lua")
local function check(ok, what) if not ok then error(what, 2) end end
check(S.plain(S.parse("a < b")) == "a < b", "a < b: " .. S.plain(S.parse("a < b")))
check(S.plain(S.parse("<3")) == "<3", "<3")
check(S.plain(S.parse("x <")) == "x <", "a < at the end")
local segs = S.parse('<c "#ff0000">love &amp; <3', { 0, 0, 0, 1 })
check(S.plain(segs) == "love & <3", "entities and the rest: " .. S.plain(segs))
local last = segs[#segs]
check(last[1] == "<3" and last[2][1] == 1 and last[2][2] == 0, "the rest takes the colour it is in")
check(S.plain(S.parse("one <b>two</b> three")) == "one two three", "closed tags dropped")
return "ok"
)lua";
    CHECK(run_gamepad_lib_script(kScript, "ui_pack_gamepad_stml_lt_test") == "ok");
}

// Popup STML tables (lib/stml_blocks.lua, the same file in both packs): the subset RoF2's ParseSTMLTable reads that
// servers send (attributes, <th>, EQEmu's <tbody>), malformed tables left as text, and a hostile popup read in one
// pass rather than one scan of the rest of the text per <table>.
TEST_CASE("UI packs: popup STML tables read what servers send and leave malformed ones as text") {
    const fs::path ui = fs::path(__FILE__).parent_path().parent_path() / "ui";
    const auto slurp = [](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    const std::string modern = slurp(ui / "modern" / "lib" / "stml_blocks.lua");
    CHECK(!modern.empty());
    CHECK(modern == slurp(ui / "gamepad" / "lib" / "stml_blocks.lua"));

    constexpr const char* kScript = R"lua(
local M = dofile("lib/stml_blocks.lua")
local function check(ok, what) if not ok then error(what, 2) end end
local function text(raw)
  local b = M.parse(raw)
  check(#b == 1 and b[1].text == raw, "left as text: " .. raw:sub(1, 80))
end
local function cells(block)
  local out = {}
  for _, row in ipairs(block.rows) do out[#out + 1] = table.concat(row, "|") end
  return table.concat(out, "/")
end

-- Attributes, <th> and the sections (EQEmu's #npcstats: zone/mob_info.cpp).
local b = M.parse('<TABLE border="1"><thead><tr><th width=40>Stat</th><TH>Value</TH></tr></thead>'
  .. '<tbody><tr align="left"><td bgcolor="#202020">AC</td><td>10</td></tr></tbody><tfoot></tfoot></TABLE>')
check(#b == 1 and b[1].columns == 2 and cells(b[1]) == "Stat|Value/AC|10", "attributes and sections")
b = M.parse("<c \"#FFFF66\">Stats</c><br><table><tbody><tr><td>HP</td><td>5</td></tr></tbody></table>")
check(#b == 2 and b[1].text == "<c \"#FFFF66\">Stats</c><br>" and cells(b[2]) == "HP|5", "tbody")

-- Malformed: an unclosed cell, row or table, text between cells, a section inside a cell, an unread tag.
text("<table><tr><td>a</tr></table>")
text("<table><tr><td>a</td></table>")
text("<table><tr><td>a</td></tr>")
text("<table><tr><td>a</td>stray<td>b</td></tr></table>")
text("<table><tr><td><tbody>a</td></tr></table>")
text("<table><caption>x</caption><tr><td>a</td></tr></table>")
text("<table></table>")

-- 64 cells in a row are read; a 65th leaves the table as text.
b = M.parse("<table><tr>" .. string.rep("<td>x</td>", 64) .. "</tr></table>")
check(#b == 1 and b[1].columns == 64, "64 cells")
text("<table><tr>" .. string.rep("<td>x</td>", 65) .. "</tr></table>")

-- A nested table: the outer one stays text, the inner one is read, and a table after it still is.
b = M.parse("<table><tr><td><table><tr><td>in</td></tr></table></td></tr></table><table><tr><td>next</td></tr></table>")
check(#b == 4 and b[1].text == "<table><tr><td>" and cells(b[2]) == "in" and b[3].text == "</td></tr></table>"
  and cells(b[4]) == "next", "nested")

-- Hostile: ~200 KB of tables that never close a cell, or close one only at the very end. (Scanning the rest of the
-- text for each <table> took seconds here.)
for _, tail in ipairs({ "", "</td>", "</td></tr></table>" }) do
  local raw = string.rep("<table><tbody><tr><td width=1>", 7000) .. tail
  local start = os.clock()
  b = M.parse(raw)
  check(os.clock() - start < 2, "hostile popup took " .. (os.clock() - start) .. " s")
  check(#b == (tail:find("table") and 2 or 1), "hostile result")
end
return "ok"
)lua";
    CHECK(run_gamepad_lib_script(kScript, "ui_pack_gamepad_stml_tables_test") == "ok");
}

// The gamepad bag grids' cell ids (lib/items.lua I.cellId) are the strings they were, made once.
TEST_CASE("UI packs (gamepad): bag grid cell ids are kept") {
    constexpr const char* kScript = R"lua(
local I = dofile("lib/items.lua")
local function check(ok, what) if not ok then error(what, 2) end end
check(I.cellId("sell", 23) == "##sell23_-1", "a bag slot")
check(I.cellId("sell", 25, 3) == "##sell25_3", "a bag's slot")
check(I.cellId("inv", 25, 3) == "##inv25_3", "another grid")
check(rawequal(I.cellId("sell", 25, 3), I.cellId("sell", 25, 3)), "kept")
return "ok"
)lua";
    CHECK(run_gamepad_lib_script(kScript, "ui_pack_gamepad_cellid_test") == "ok");
}

// The gamepad windows keep what is costly to read or lay out (a raid's members, the raid's and the guild's message of
// the day, the /who answer, an item's wrapped description) between frames: a change still shows. The stock windows'
// texts and lists are stood in for from Lua, and every text the theme draws is recorded.
TEST_CASE("UI packs (gamepad): cached window contents follow a change") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "gamepad";
    hailstone::test::ScratchDir dir_scratch("ui_pack_gamepad_caches_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "gamepad", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "gamepad");
        s.frames(2);
        lua_State* L = s.lua.state();
        auto run = [&](const char* code) {
            INFO(code);
            REQUIRE(luaL_dostring(L, code) == LUA_OK);
        };
        auto saw = [&](const char* needle) {
            lua_getglobal(L, "saw");
            lua_pushstring(L, needle);
            REQUIRE(lua_pcall(L, 1, 1, 0) == LUA_OK);
            bool found = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            return found;
        };
        run(R"lua(
            seen = {}
            -- (every theme text, T.text, T.draw and T.textIn alike, ends in ui.DrawText(x, y, r, g, b, a, text))
            local DrawText = ui.DrawText
            ui.DrawText = function(...)
              seen[#seen + 1] = select(7, ...)
              return DrawText(...)
            end
            function saw(needle)
              for _, s in ipairs(seen) do
                if type(s) == "string" and s:find(needle, 1, true) then return true end
              end
              return false
            end
            fake = {
              raidMotd = "Raid message one", members = { { "1", "Alpha", "50", "Warrior", "", "Leader" } },
              guildMotd = "Guild message one", desc = "The first description",
              who = 1, players = { { name = "Echo", line = "[50 Warrior] Echo (Human) <Guild> ZONE: qeynos" } },
            }
            local ListInfo, ListRow, WindowText, WhoResults = hs.ListInfo, hs.ListRow, hs.WindowText, hs.WhoResults
            hs.ListInfo = function(win, child, ...)
              if win == "RaidWindow" then
                if child == "RAID_PlayerList" then return #fake.members, 6, -1 end
                return 0, 0, -1
              end
              if win == "GuildManagementWnd" and child == "MemberList" then return 0, 0, -1 end
              return ListInfo(win, child, ...)
            end
            hs.ListRow = function(win, child, row, ...)
              if win == "RaidWindow" then
                return child == "RAID_PlayerList" and fake.members[row + 1] or nil
              end
              return ListRow(win, child, row, ...)
            end
            hs.WindowText = function(win, child, ...)
              if win == "RaidWindow" and child == "MOTDViewer" then return fake.raidMotd end
              if win == "GuildManagementWnd" and child == "MOTDViewer" then return fake.guildMotd end
              if win == "GuildManagementWnd" and child == "GuildName" then return "The Guild" end
              if win == "ItemDisplayWindow" and child == "ItemDescription" then return fake.desc end
              return WindowText(win, child, ...)
            end
            hs.WhoResults = function() return fake.who, fake.players end
            -- (the stock guild and item windows show only in a guild / for an item: shown here on asking)
            fake.shown = {}
            local ReplaceWindow, WindowShown = hs.ReplaceWindow, hs.WindowShown
            hs.ReplaceWindow = function(win, ...)
              local r = ReplaceWindow(win, ...)
              if fake.shown[win] ~= nil then return fake.shown[win] end
              return r
            end
            hs.WindowShown = function(win)
              local r = WindowShown(win)
              if fake.shown[win] ~= nil then return fake.shown[win] end
              return r
            end
        )lua");

        // the raid: its message of the day at once, a new member at once (the row count), a renamed one within 0.25 s
        run("hs.ShowWindow('RaidWindow', true) seen = {}");
        s.frames(2);
        CHECK(saw("MOTD: Raid message one"));
        CHECK(saw("Alpha"));
        run("fake.raidMotd = 'Raid message two' "
            "fake.members[2] = { '2', 'Bravo', '40', 'Cleric', '', '' } seen = {}");
        s.frames(1);
        CHECK(saw("MOTD: Raid message two"));
        CHECK_FALSE(saw("Raid message one"));
        CHECK(saw("Bravo"));
        run("fake.members[2] = { '2', 'Charlie', '40', 'Cleric', '', '' }");
        s.frames(20);
        run("seen = {}");
        s.frames(1);
        CHECK(saw("Charlie"));
        CHECK_FALSE(saw("Bravo"));
        run("hs.ShowWindow('RaidWindow', false)");

        // the guild's message of the day
        run("fake.shown.GuildManagementWnd = true seen = {}");
        s.frames(2);
        CHECK(saw("Guild message one"));
        run("fake.guildMotd = '<b>Guild</b>   message two' seen = {}");
        s.frames(1);
        CHECK(saw("Guild message two"));
        CHECK_FALSE(saw("Guild message one"));
        run("fake.shown.GuildManagementWnd = false");

        // the /who answer: each row's line cut down to what follows the name; another reply within 0.25 s
        run("GPSTATE.whoOpen = true seen = {}");
        s.frames(2);
        CHECK(saw("Echo"));
        CHECK(saw("(Human) <Guild> ZONE: qeynos"));
        run("fake.who = 2 fake.players = { { name = 'Delta', line = '[60 Cleric] Delta (Erudite)' } }");
        s.frames(20);
        run("seen = {}");
        s.frames(1);
        CHECK(saw("Delta"));
        CHECK(saw("(Erudite)"));
        CHECK_FALSE(saw("Echo"));
        run("GPSTATE.whoOpen = false");

        // an item's description, wrapped once while it stays the same
        run("fake.shown.ItemDisplayWindow = true seen = {}");
        s.frames(2);
        CHECK(saw("The first description"));
        run("fake.desc = 'Another description' seen = {}");
        s.frames(1);
        CHECK(saw("Another description"));
        CHECK_FALSE(saw("The first description"));
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("UI packs (gamepad): Final Result stays reachable and dispatches only an enabled explicit click") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "gamepad";
    hailstone::test::ScratchDir scratch("ui_pack_evolving_final_test");
    fs::path dir = scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "gamepad", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "gamepad");
        s.frames(2);
        lua_State* L = s.lua.state();
        auto run = [&](const char* code) {
            INFO(code);
            const int result = luaL_dostring(L, code);
            INFO((lua_tostring(L, -1) ? lua_tostring(L, -1) : ""));
            REQUIRE(result == LUA_OK);
            lua_settop(L, 0);
        };
        run(R"lua(
            finalTest = { shown = true, enabled = true, click = false, calls = 0, drawn = 0 }
            local WindowShown, ChildShown = hs.WindowShown, hs.ChildShown
            local WindowText, ChildState, ClickChild = hs.WindowText, hs.ChildState, hs.ClickChild
            hs.WindowShown = function(win, ...)
              if win == "ItemDisplayWindow" then return true end
              return WindowShown(win, ...)
            end
            hs.ChildShown = function(win, child, ...)
              if win == "ItemDisplayWindow" and child == "Hailstone_EvolvingFinalResult" then
                return finalTest.shown
              end
              return ChildShown(win, child, ...)
            end
            hs.ChildState = function(win, child, ...)
              if win == "ItemDisplayWindow" and child == "Hailstone_EvolvingFinalResult" then
                return finalTest.enabled
              end
              return ChildState(win, child, ...)
            end
            hs.WindowText = function(win, child, ...)
              if win == "ItemDisplayWindow" then
                if child == "Hailstone_EvolvingFinalResult" then return "Final Result" end
                if child == "ItemDescription" then return ("A very long item description. "):rep(500) end
              end
              return WindowText(win, child, ...)
            end
            hs.ClickChild = function(win, child, button, ...)
              if win == "ItemDisplayWindow" and child == "Hailstone_EvolvingFinalResult" then
                assert(button == "left")
                finalTest.calls = finalTest.calls + 1
                return true
              end
              return ClickChild(win, child, button, ...)
            end
            local InvisibleButton = ui.InvisibleButton
            ui.InvisibleButton = function(id, w, h, flags, ...)
              if id == "##evolvingfinal" then
                finalTest.drawn = finalTest.drawn + 1
                assert((flags & ui.ButtonFlags.EnableNav) ~= 0, "Final Result must allow pad focus")
                local x, y = ui.GetCursorScreenPos()
                local wx, wy = ui.GetWindowPos()
                local ww, wh = ui.GetWindowSize()
                assert(x >= wx and y >= wy and x + w <= wx + ww and y + h <= wy + wh,
                  "Final Result must remain inside the item window")
                InvisibleButton(id, w, h, flags, ...)
                return finalTest.click
              end
              return InvisibleButton(id, w, h, flags, ...)
            end
        )lua");
        s.frames(2);
        run("assert(finalTest.drawn == 2 and finalTest.calls == 0)");
        run("finalTest.click = true");
        s.frames(1);
        run("assert(finalTest.calls == 1)");
        run("finalTest.enabled = false");
        s.frames(1);
        run("assert(finalTest.calls == 1) finalTest.shown = false finalTest.drawn = 0");
        s.frames(1);
        run("assert(finalTest.calls == 1 and finalTest.drawn == 0)");
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// The modern item display reads the window's info lines and stat grid again only when the item changes (or every
// quarter second): another item shows at once, and what it draws is what the window holds.
TEST_CASE("UI packs (modern): an item display draws the shown item's stats, and another item at once") {
    fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / "modern";
    hailstone::test::ScratchDir dir_scratch("ui_pack_item_display_test");
    fs::path dir = dir_scratch.path();
    fs::create_directories(dir / "game");
    fs::copy(src, dir / "modern", fs::copy_options::recursive);
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
    {
        Session s(dir, "modern");
        auto item = [](std::uint32_t id, const char* name, int str) {
            net::Item it;
            it.id = id;
            it.name = name;
            it.classes = 0xffff;
            it.races = 0xffff;
            it.equip_slots = 1u << 13;
            it.damage = 4;
            it.delay = 29;
            it.astr = static_cast<std::int8_t>(str);
            it.ac = str * 2;
            return it;
        };
        std::vector<net::Item> displays{item(1001, "Test Sword", 3)};
        std::vector<std::uint64_t> ids{1};
        s.host.item_displays = &displays;
        s.host.item_display_ids = &ids;
        lua_State* L = s.lua.state();
        // Every string drawn (DrawText, TextColored); then: is each stat value the window holds among them?
        REQUIRE(luaL_dostring(L, R"(
            test_seen = {}
            local dt, tc = ui.DrawText, ui.TextColored
            ui.DrawText = function(...) local a = { ... } test_seen[tostring(a[#a])] = true return dt(...) end
            ui.TextColored = function(...) local a = { ... } test_seen[tostring(a[5])] = true return tc(...) end
            function test_check()
              local n, missing = 0, {}
              for c = 1, 3 do
                for r = 1, 26 do
                  local st = hs.WindowText("ItemDisplayWindow", "IDW_Row" .. r .. "Col" .. c .. "Stat") or ""
                  local v = (hs.WindowText("ItemDisplayWindow", "IDW_Row" .. r .. "Col" .. c .. "Value") or "")
                    :match("^%s*(.-)%s*$")
                  if hs.ChildShown("ItemDisplayWindow", "IDW_Row" .. r .. "Col" .. c .. "Stat") ~= false
                      and st:match("%S") and v ~= "" then
                    n = n + 1
                    if not test_seen[v] then missing[#missing + 1] = v end
                  end
                end
              end
              return n, table.concat(missing, ",")
            end)") == LUA_OK);
        auto check = [&](const char* name) {
            REQUIRE(luaL_dostring(L, "test_seen = {}") == LUA_OK);
            s.frames(1);
            REQUIRE(luaL_dostring(L, "return test_check()") == LUA_OK);
            CHECK(lua_tointeger(L, -2) > 0);
            CHECK(std::string(lua_tostring(L, -1)) == "");
            lua_settop(L, 0);
            REQUIRE(luaL_dostring(L, (std::string("return test_seen['") + name + "'] == true").c_str()) == LUA_OK);
            CHECK(lua_toboolean(L, -1));
            lua_settop(L, 0);
        };
        s.frames(2);
        check("Test Sword");
        // another item in the same window: drawn the next frame
        displays[0] = item(1002, "Other Blade", 7);
        check("Other Blade");
        // one of the same name with other stats: drawn within the quarter second the parse is kept
        displays[0] = item(1003, "Other Blade", 11);
        s.frames(20);
        check("Other Blade");
        CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
    }
    ImGui::DestroyContext(ctx);
}

// Hailstone's item comparison (docs/item-compare.md): a linked item's display says what wearing it would change,
// in both packs, and the Options switch turns it off.
TEST_CASE("UI packs: the item display compares the item with what you wear") {
    for (const char* pack : {"modern", "gamepad"}) {
        CAPTURE(pack);
        fs::path src = fs::path(__FILE__).parent_path().parent_path() / "ui" / pack;
        hailstone::test::ScratchDir dir_scratch("ui_pack_item_compare_test");
        fs::path dir = dir_scratch.path();
        fs::create_directories(dir / "game");
        std::ofstream(dir / "game" / "eqstr_us.txt") << "3029 (Combat)\n3036 Effect: %1 %2\n";
        fs::copy(src, dir / pack, fs::copy_options::recursive);
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // 1.92: the atlas is built by NewFrame
        {
            Session s(dir, pack);
            namespace mech = game::mechanics;
            mech::GameTables tables;
            tables.class_levels.parse("60^1^400^0^300^3^6^0.9^0^1.25\n");
            tables.skill_caps.parse("1^15^60^210^0\n");
            mech::CharacterSheet sheet;
            sheet.level = sheet.highest_level = 60;
            sheet.base_str = sheet.base_sta = sheet.base_cha = sheet.base_dex = sheet.base_int = sheet.base_agi =
                sheet.base_wis = 75;
            sheet.buffs.resize(mech::kPlayerBuffSlots);
            net::Item old_helm;
            old_helm.name = "Old Helm";
            old_helm.classes = old_helm.races = 0xffff;
            old_helm.equip_slots = 1u << 2;
            old_helm.hp = 10;
            sheet.possessions[2] = old_helm;
            mech::Character ch(sheet, tables, [](std::int32_t) -> const game::SpellData* { return nullptr; });
            ch.process_all_stats();
            s.host.character = &ch;
            int option = 1;
            s.host.find_option = [&](std::string_view o) -> std::optional<int> { return o == "item_compare" ? option : 1; };

            net::Item helm = old_helm;
            helm.id = 1001;
            helm.name = "Iron Helm";
            helm.hp = 60;
            helm.effects[net::kEffectProc].effect = -1;
            helm.effects[net::kEffectProc].name = "Frost & Fire";
            std::vector<net::Item> displays{helm};
            std::vector<std::uint64_t> ids{1};
            s.host.item_displays = &displays;
            s.host.item_display_ids = &ids;
            lua_State* L = s.lua.state();
            REQUIRE(luaL_dostring(L, R"(
                test_seen = {}
                local dt, tc, tw = ui.DrawText, ui.TextColored, ui.TextWrapped
                ui.DrawText = function(...) local a = { ... } test_seen[tostring(a[#a])] = true return dt(...) end
                ui.TextColored = function(...) local a = { ... } test_seen[tostring(a[5])] = true return tc(...) end
                ui.TextWrapped = function(t, ...) test_seen[tostring(t)] = true return tw(t, ...) end)") ==
                    LUA_OK);
            auto seen = [&](const std::string& text) {
                REQUIRE(luaL_dostring(L, ("return test_seen['" + text + "'] == true").c_str()) == LUA_OK);
                bool b = lua_toboolean(L, -1);
                lua_settop(L, 0);
                return b;
            };
            s.frames(3);
            // The HP change for you beside the item's HP in the stat grid, not as a line of its own below it.
            const std::string heading = pack == std::string("modern") ? "vs your Old Helm (Head)"
                                                                      : "Compared with your Old Helm (Head)";
            CHECK(seen(heading));
            CHECK(seen("Upgrade"));
            CHECK(seen("60"));
            CHECK(seen("+50"));
            CHECK(seen("Gains Proc: Frost & Fire"));
            CHECK(seen("Effect: Frost & Fire (Combat)"));
            CHECK_FALSE(seen("+50 HP"));
            CHECK_FALSE(seen("+50 HP (" + std::to_string(ch.max_hp(true)) + " to " + std::to_string(ch.max_hp(true) + 50) + ")"));
            CHECK(ImGui::FindWindowByName("UI errors") == nullptr);

            if (pack == std::string("modern")) {
                // The tooltip: the verdict under the name, the HP change beside the item's HP, the rest as "+n NAME".
                REQUIRE(luaL_dostring(L, R"(
                    test_seen = {}
                    local items = dofile("lib/items.lua")
                    local c = { usable = true, whyNot = "", notes = {}, slots = { { slot = 2, slotName = "Head",
                      current = { "Old Helm" }, verdict = "upgrade", score = 60, gained = {}, lost = {},
                      changes = { { key = "hp", name = "HP", before = 100, after = 150, delta = 50, weight = 1 },
                                  { key = "str", name = "STR", before = 80, after = 85, delta = 5, weight = 0.5 } } } } }
                    local dt, tx, td = ui.DrawText, ui.Text, ui.TextDisabled
                    ui.Text = function(t, ...) test_seen[tostring(t)] = true return tx(t, ...) end
                    ui.TextDisabled = function(t, ...) test_seen[tostring(t)] = true return td(t, ...) end
                    ui.window("Compare tooltip test", function()
                      items.Tooltip({ name = "Iron Helm", hp = 60, weight = 3 }, "Head", c)
                    end))") == LUA_OK);
                s.frames(2);
                CHECK(seen("Upgrade"));
                CHECK(seen("HP 60"));
                CHECK(seen("+50"));
                CHECK(seen("+5 STR"));
                CHECK_FALSE(seen("+50 HP"));
                CHECK(seen("vs your Old Helm (Head)"));
                CHECK(ImGui::FindWindowByName("UI errors") == nullptr);
                REQUIRE(luaL_dostring(L, "ui.window('Compare tooltip test', function() end)") == LUA_OK);
            }

            option = 0;
            REQUIRE(luaL_dostring(L, "test_seen = {}") == LUA_OK);
            s.frames(2);
            CHECK_FALSE(seen(heading));
            CHECK_FALSE(seen("+50"));
            CHECK_FALSE(seen("Gains Proc: Frost & Fire"));
            CHECK(seen("Effect: Frost & Fire (Combat)"));
        }
        ImGui::DestroyContext(ctx);
    }
}
