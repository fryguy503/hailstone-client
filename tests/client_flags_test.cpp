// The client's command-line flags: the game window at start (app/window_desc.h: the saved window, unless
// --window-size asked for a size) and the display flags as settings for the run (app/display_flags.h).
#include <doctest/doctest.h>

#include "app/display_flags.h"
#include "app/window_desc.h"

using namespace hailstone;

namespace {
Config saved_window() {
    Config c;
    c.parse("window.width=1920\nwindow.height=1080\nwindow.x=40\nwindow.y=50\nwindow.maximized=1\n");
    return c;
}
} // namespace

TEST_CASE("window_desc: without --window-size the saved window comes back") {
    const Config c = saved_window();
    app::ClientOptions o;
    platform::WindowDesc d = app::window_desc(o, &c);
    CHECK(d.width == 1920);
    CHECK(d.height == 1080);
    REQUIRE(d.position);
    CHECK(d.position->first == 40);
    CHECK(d.position->second == 50);
    CHECK(d.maximized);
}

TEST_CASE("window_desc: --window-size 1280x720 is a size asked for, not the default") {
    const Config c = saved_window();
    app::ClientOptions o;
    o.window_width = 1280;
    o.window_height = 720;
    o.window_size_given = true;
    platform::WindowDesc d = app::window_desc(o, &c);
    CHECK(d.width == 1280);
    CHECK(d.height == 720);
    CHECK_FALSE(d.position);
    CHECK_FALSE(d.maximized);
}

TEST_CASE("window_desc: another --window-size and no saved settings") {
    app::ClientOptions o;
    o.window_width = 800;
    o.window_height = 600;
    o.window_size_given = true;
    const Config c = saved_window();
    CHECK(app::window_desc(o, &c).width == 800);
    app::ClientOptions plain;
    CHECK(app::window_desc(plain, nullptr).width == 1280);
    CHECK(app::window_desc(plain, nullptr).height == 720);
}

TEST_CASE("display flags: the client takes them as its display settings") {
    using app::display_flag_setting;
    using Setting = std::pair<std::string, std::string>;
    CHECK(display_flag_setting("--lighting", "classic") == Setting{"display.lighting", "classic"});
    CHECK(display_flag_setting("--lighting", "smooth") == Setting{"display.lighting", "smooth"});
    CHECK(display_flag_setting("--lighting", "modern") == Setting{"display.lighting", "modern"});
    CHECK_FALSE(display_flag_setting("--lighting", "none")); // full-bright: the viewer's alone
    CHECK(display_flag_setting("--msaa", "2") == Setting{"display.msaa", "2"});
    CHECK(display_flag_setting("--tonemap", "aces") == Setting{"display.tonemap", "aces"});
    CHECK(display_flag_setting("--exposure", "-0.5") == Setting{"display.exposure", "-0.5"});
    CHECK(display_flag_setting("--shadows", "0") == Setting{"display.shadows", "0"});
    CHECK(display_flag_setting("--light-shadows", "2") == Setting{"display.light_shadows", "2"});
    CHECK(display_flag_setting("--no-ao") == Setting{"display.ao", "0"});
    CHECK(display_flag_setting("--depth-prepass") == Setting{"display.depth_prepass", "1"});
    CHECK(display_flag_setting("--bloom", "0.2") == Setting{"display.bloom", "0.2"});
    CHECK(display_flag_setting("--render-scale", "67") == Setting{"display.render_scale", "67"});
    CHECK(display_flag_setting("--sharpness", "0.8") == Setting{"display.sharpness", "0.8"});
    CHECK(display_flag_setting("--upscaler", "taa") == Setting{"display.upscaler", "taa"});
    CHECK(display_flag_setting("--frame-gen") == Setting{"display.frame_gen", "1"});
    CHECK(display_flag_setting("--view-distance", "4") == Setting{"display.view_distance", "4"});
    CHECK_FALSE(display_flag_setting("--max-fps", "60")); // its own option (ClientOptions::max_fps)
    CHECK_FALSE(display_flag_setting("--gpu-timing"));
    CHECK_FALSE(display_flag_setting("--zone", "qeynos2"));
}
