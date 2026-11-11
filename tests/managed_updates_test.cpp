#include <doctest/doctest.h>

#include "app/update_install.h"
#include "app/updater.h"
#include "core/config.h"
#include "core/distribution.h"
#include "scratch_dir.h"

using namespace hailstone;

TEST_CASE("managed distributions cannot replace themselves through the generic updater") {
    REQUIRE(distribution::managed());
    test::ScratchDir dir("managed_updates");
    Config config;
    config.set("update.check", "1");
    config.set("update.download", "1");
    config.set("update.install_on_quit", "1");
    config.set("update.feed", "https://example.invalid/unrestricted");
    const auto settings = app::update_settings(config);
    CHECK_FALSE(settings.check);
    CHECK_FALSE(settings.download);
    CHECK_FALSE(app::update_installs_at_quit(settings, true));
    unsigned fetches = 0;
    app::UpdaterEnv env;
    env.install_dir = dir.path();
    env.fetch = [&](const auto&, const auto&, const auto&, const auto&) {
        ++fetches;
        return platform::HttpGetResult{};
    };
    app::Updater updater(env);
    updater.check();
    updater.wait();
    CHECK(fetches == 0);
    CHECK(updater.status().state == app::UpdateState::Failed);
    std::string why;
    CHECK_FALSE(app::stage_update(dir.path(), "99.0.0", {}, &why));
    CHECK_FALSE(std::filesystem::exists(app::update_dir(dir.path()) / "ready.txt"));
    CHECK_FALSE(app::install_staged_update(dir.path(), "1.0.0").ok);
    CHECK_FALSE(app::update_at_start(dir.path(), "1.0.0").restart);
}
