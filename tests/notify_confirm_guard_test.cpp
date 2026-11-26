// The destroy guard on /notify and the Window methods (app::run_notify_command, run_window_act): code (a script, a
// plugin: plugin::caller_name() not empty) cannot press ConfirmationDialogBox's Yes while it asks a destroy's or a
// guild disband's question, as through hs.ClickChild and hs.WindowButton (refuse_code_destroy_yes). Any other
// question, and the player's own typed /notify, still take the Yes. No ImGui context and no Lua here.
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>

#include "app/lua_ui.h"
#include "mq/parse.h"
#include "mq/tlo.h"
#include "mq_test_world.h"
#include "plugin/policy.h"

using namespace hailstone;
using A = net::GameConnectionTestAccess;

namespace {

struct Rig {
    mq_test::World world{"hailstone_notify_confirm_guard"};
    app::UiHost host;
    std::unique_ptr<app::UiApi> api;
    mq::Registry reg;
    mq::Context ctx;

    Rig() {
        host.in_game = host.ui_ready = true;
        host.conn = world.conn.get();
        api = std::make_unique<app::UiApi>(host);
        mq::register_game_tlos(reg);
        ctx.registry = &reg;
        ctx.conn = world.conn.get();
        // As the client wires them (app/client_app_internal.h).
        ctx.window_child = [this](std::string_view w, std::string_view c, bool rows) { return api->window_child(w, c, rows); };
        ctx.window_act = [this](std::string_view w, std::string_view c, std::string_view action) {
            return app::run_window_act(host, api.get(), w, c, action);
        };
    }
    void ask(std::uint32_t id) {
        net::Confirmation q;
        q.id = id;
        q.text = "A question?";
        q.value = "Surname";
        A::confirmation(*world.conn) = q;
    }
    bool asking(std::uint32_t id) const {
        const auto& c = world.conn->confirmation();
        return c && c->id == id;
    }
    bool notify(std::string_view args) { return app::run_notify_command(host, api.get(), args); }
    std::string left_mouse_up() {
        return mq::parse_macro_data(ctx, "${Window[ConfirmationDialogBox].Child[Yes_Button].LeftMouseUp}");
    }
};

} // namespace

TEST_CASE("notify guard: a script's /notify Yes on a destroy or a guild disband is refused") {
    for (const std::uint32_t id : {net::kConfirmDestroy, net::kConfirmGuildDelete}) {
        CAPTURE(id);
        Rig r;
        r.ask(id);
        REQUIRE(app::destructive_confirmation_up(r.host));
        {
            plugin::CallerScope script(plugin::kCapAll, "a script");
            CHECK_FALSE(r.notify("ConfirmationDialogBox Yes_Button leftmouseup"));
            CHECK(r.asking(id));
            // MQ's names for the button, too.
            CHECK_FALSE(r.notify("ConfirmationDialogBox CD_Yes_Button leftmouseup"));
            CHECK(r.asking(id));
            // No still answers.
            CHECK(r.notify("ConfirmationDialogBox No_Button leftmouseup"));
            CHECK_FALSE(r.world.conn->confirmation());
        }
        // The player's own typed /notify (no caller): the Yes answers.
        r.ask(id);
        CHECK(r.notify("ConfirmationDialogBox Yes_Button leftmouseup"));
        CHECK_FALSE(r.world.conn->confirmation());
    }
}

TEST_CASE("notify guard: a script's Window Child LeftMouseUp on a destroy or a guild disband's Yes is refused") {
    for (const std::uint32_t id : {net::kConfirmDestroy, net::kConfirmGuildDelete}) {
        CAPTURE(id);
        Rig r;
        r.ask(id);
        {
            plugin::CallerScope script(plugin::kCapAll, "a script");
            CHECK(r.left_mouse_up() == "FALSE");
            CHECK(r.asking(id));
        }
        // The player's line read with no caller (a typed /invoke ${...}): it answers.
        CHECK(r.left_mouse_up() == "TRUE");
        CHECK_FALSE(r.world.conn->confirmation());
    }
}

TEST_CASE("notify guard: an ordinary question still takes a script's Yes") {
    Rig r;
    r.ask(net::kConfirmSurname);
    CHECK_FALSE(app::destructive_confirmation_up(r.host));
    {
        plugin::CallerScope script(plugin::kCapAll, "a script");
        CHECK(r.notify("ConfirmationDialogBox Yes_Button leftmouseup"));
        CHECK_FALSE(r.world.conn->confirmation());
        r.ask(net::kConfirmSurname);
        CHECK(r.left_mouse_up() == "TRUE");
        CHECK_FALSE(r.world.conn->confirmation());
    }
}
