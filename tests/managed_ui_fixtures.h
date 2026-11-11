#pragma once

#include <array>
#include <string_view>

#include "core/distribution.h"
#include "core/sha256.h"

namespace hailstone::test::managed_ui {

inline constexpr std::string_view entry = R"lua(local reader_called = false
local loaded, why = load('return 99')
local reader = load(function() reader_called = true; return 'return 99' end)
managed_probe = {
  managed = ui.ManagedDistribution(), module = dofile('lib/managed_value.lua'),
  load_denied = loaded == nil and type(why) == 'string', reader_denied = reader == nil and not reader_called,
  test_read = ui.GetSetting('test.click.ManagedFixture'), test_write = ui.SetSetting('test.show', 'ManagedFixture'),
  layout_read = ui.GetSetting('hud.alpha'), layout_write = ui.SetSetting('hud.alpha', '0.5'),
  no_require = require == nil and package == nil,
}
ui.window('ManagedFixture', function() end)
)lua";
inline constexpr std::string_view module = "return 17\n";
inline constexpr std::string_view plugin = "managed_plugin = dofile('lib/managed_value.lua')\n";

// Included only by the managed test executable's startup policy, never the client manifest.
inline auto approved_sources() {
    return std::array{
        distribution::UiSource{"modern/managed_fixture.lua", sha256(entry)},
        distribution::UiSource{"modern/lib/managed_value.lua", sha256(module)},
        distribution::UiSource{"plugin:managedfixture/modern/managed_window.lua", sha256(plugin)},
    };
}

} // namespace hailstone::test::managed_ui
