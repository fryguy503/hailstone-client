#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hailstone::app {

struct ServerManagementField {
    std::string section, key, value;
};
using ServerManagementEdit = ServerManagementField;

struct ServerManagementCommand {
    std::string name, description, origin;
    std::vector<std::string> aliases, groups;
};

// The config snapshot and locally registered commands have independent revisions.
struct ServerManagementView {
    std::string revision, file_revision, message;
    std::uint32_t serial = 0;
    std::uint32_t catalog_serial = 0;
    int minimum_status = 200;
    bool saving = false;
    std::vector<ServerManagementField> fields;
    std::vector<ServerManagementCommand> commands;
};

} // namespace hailstone::app
