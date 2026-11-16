// The bug report window's Send (app/lua_ui_dialogs.cpp BugReportWindow) over the hs API: the report is written by
// UiHost::save_bug_report's future, the window says it is being saved until that is ready, and a Send made while an
// earlier report is still being written waits for it and is the one the window shows.
#include <doctest/doctest.h>

#include <future>
#include <string>
#include <vector>

#include <imgui.h>
#include <lua.hpp>

#include "app/lua_ui.h"
#include "mock_server/harness.h"

using namespace hailstone;
using namespace hailstone::test;

namespace {

// The chunk's first result as text ("" for none).
std::string run(lua_State* L, const std::string& code) {
    const int top = lua_gettop(L);
    std::string out;
    if (luaL_dostring(L, code.c_str()) != LUA_OK) out = std::string("error: ") + lua_tostring(L, -1);
    else if (lua_gettop(L) > top) {
        const char* s = lua_isboolean(L, top + 1) ? nullptr : lua_tostring(L, top + 1);
        out = s ? s : (lua_isboolean(L, top + 1) ? (lua_toboolean(L, top + 1) ? "true" : "false") : "nil");
    }
    lua_settop(L, top);
    return out;
}

// Each Send's report, with the promise the test sets when "written".
struct Saves {
    std::vector<app::BugReport> reports;
    std::vector<std::promise<std::optional<app::SavedBugReport>>> promises;
    std::future<std::optional<app::SavedBugReport>> save(app::BugReport r) {
        reports.push_back(std::move(r));
        promises.emplace_back();
        return promises.back().get_future();
    }
    void done(std::size_t i, const char* name) {
        app::SavedBugReport s;
        s.zip = std::filesystem::path("reports") / name;
        s.size = 1000;
        promises[i].set_value(s);
    }
};

struct Window {
    ImGuiContext* ctx = ImGui::CreateContext();
    EmptyGameDir dir;
    GameDir game{dir.path()};
    net::GameConnection conn{game, quick_options()};
    app::UiHost host;
    Saves saves;
    std::unique_ptr<app::UiApi> api;
    lua_State* L = luaL_newstate();
    Window() {
        host.in_game = host.ui_ready = true;
        host.conn = &conn;
        host.save_bug_report = [this](app::BugReport r) { return saves.save(std::move(r)); };
        api = std::make_unique<app::UiApi>(host);
        luaL_openlibs(L);
        lua_newtable(L);
        api->register_functions(L);
        lua_setglobal(L, "hs");
    }
    ~Window() {
        lua_close(L);
        api.reset();
        ImGui::DestroyContext(ctx);
    }
    std::string shown() { return run(L, "return hs.WindowShown('BugReportWindow')"); }
    std::string saved_name() { return run(L, "return (hs.WindowText('BugReportWindow', 'SavedName'))"); }
    std::string send(const char* text) {
        run(L, std::string("hs.SetWindowText('BugReportWindow', 'Edit', '") + text + "')");
        return run(L, "return hs.ClickChild('BugReportWindow', 'SendButton', 'left')");
    }
};

} // namespace

TEST_CASE("Bug report window: Send says the report is being saved, then shows its file") {
    Window w;
    w.conn.set_bug_report_open(true);
    CHECK(w.shown() == "true");
    CHECK(w.saved_name() == "");
    CHECK(w.send("the door is stuck") == "true");
    REQUIRE(w.saves.reports.size() == 1);
    CHECK(w.saves.reports[0].text == "the door is stuck");
    CHECK(w.shown() == "true");
    CHECK(w.saved_name() == "(being saved)");
    w.saves.done(0, "bug-1.zip");
    CHECK(w.shown() == "true");
    CHECK(w.saved_name() == "bug-1.zip");
}

TEST_CASE("Bug report window: a report that could not be written closes the window") {
    Window w;
    w.conn.set_bug_report_open(true);
    CHECK(w.shown() == "true");
    w.send("crash at the bank");
    CHECK(w.shown() == "true");
    w.saves.promises[0].set_value(std::nullopt);
    CHECK(w.shown() == "false");
    CHECK_FALSE(w.conn.bug_report_open());
}

TEST_CASE("Bug report window: a Send while an earlier report is still being written shows its own file") {
    Window w;
    w.conn.set_bug_report_open(true);
    CHECK(w.shown() == "true");
    w.send("first");
    // Closed while the first is written, opened again, and another sent.
    w.conn.set_bug_report_open(false);
    CHECK(w.shown() == "false");
    w.conn.set_bug_report_open(true);
    CHECK(w.shown() == "true");
    CHECK(w.saved_name() == "");
    w.send("second");
    CHECK(w.saves.reports.size() == 1); // one written at a time
    CHECK(w.saved_name() == "(being saved)");
    w.saves.done(0, "bug-first.zip");
    CHECK(w.shown() == "true");
    CHECK(w.saved_name() == "(being saved)"); // not the first report's file
    REQUIRE(w.saves.reports.size() == 2);   // the second is being written now
    CHECK(w.saves.reports[1].text == "second");
    w.saves.done(1, "bug-second.zip");
    CHECK(w.shown() == "true");
    CHECK(w.saved_name() == "bug-second.zip");
}

// A Send that waits for an earlier report is filled at its Send (UiHost::bug_report_now: the zone, the place, the
// screenshot's bytes), not when its turn to be written comes, by which time the player may have zoned.
TEST_CASE("Bug report window: a queued Send keeps the facts of the moment it was sent") {
    Window w;
    int now = 0;
    w.host.bug_report_now = [&](app::BugReport& r) { r.facts.emplace_back("Moment", std::to_string(++now)); };
    w.conn.set_bug_report_open(true);
    CHECK(w.shown() == "true");
    w.send("first");
    w.send("second");
    CHECK(now == 2); // both filled at their Send
    now = 100;       // what a late fill would see
    w.saves.done(0, "bug-first.zip");
    CHECK(w.shown() == "true");
    REQUIRE(w.saves.reports.size() == 2);
    auto moment = [](const app::BugReport& r) {
        for (const auto& [k, v] : r.facts)
            if (k == "Moment") return v;
        return std::string();
    };
    CHECK(moment(w.saves.reports[0]) == "1");
    CHECK(moment(w.saves.reports[1]) == "2");
}
