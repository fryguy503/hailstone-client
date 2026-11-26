#pragma once

#include <cstdint>
#include <optional>
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

struct ServerManagementTlo {
    std::string name, description, origin;
};

struct ServerManagementAddonTlos {
    std::string repo, version, status;
    std::optional<std::vector<std::string>> required_tlos, suggested_tlos;
};

// The config snapshot and locally registered commands and TLOs have independent revisions.
struct ServerManagementView {
    std::string revision, file_revision, message;
    std::uint32_t serial = 0;
    std::uint32_t catalog_serial = 0;
    int minimum_status = 200;
    bool saving = false;
    bool reload_required = false;
    bool reloading = false;
    bool reloaded = false;
    bool paused = false;
    std::vector<ServerManagementField> fields;
    std::vector<ServerManagementCommand> commands;
    std::vector<ServerManagementTlo> tlos;
};

} // namespace hailstone::app
