#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "app/addon_install.h"
#include "plugin/policy.h"

namespace hailstone::app {

// External providers load before the server is known and cannot be exchanged under Vulkan. An opted-in server
// refuses entry if the active provider is disallowed, changed, or different from a required provider. The loaded
// folder may be a copy: checking only the current installation would accept an update that is not running yet.
std::string addon_upscaler_problem(const plugin::AddonRules& rules, const AddonRegistry& registry,
                                  std::string_view active_name, const std::filesystem::path& loaded_folder,
                                  const std::function<bool()>& canceled = {});

} // namespace hailstone::app
