#pragma once

// The player's word on a server's compiled plugins (docs/plugins.md "The server's compiled plugins ask first"). A
// library the server sent (servers/<profile>/plugins/) runs as native code with the player's full rights, so it
// loads only once the player allowed those exact bytes for that server: the answers are kept by SHA-256 in
// servers/<profile>/plugin_approvals.txt, which the server's files cannot reach (net::server_file_destination writes
// only hailstone.cfg, scripts/, plugins/ and assets/ there). A plugin whose bytes change asks again.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/sha256.h"

namespace hailstone::plugin {

inline constexpr std::string_view kApprovalsName = "plugin_approvals.txt";

enum class Consent : std::uint8_t { Unasked, Allowed, Denied };

struct PluginApproval {
    std::string name;     // the plugin's name (plugin_name), for the player and for revoking by name
    Sha256Digest sha256{}; // the bytes the answer is for
    bool allowed = false;
};

// The answers kept for the server whose folder is `profile_dir` (servers/<profile>/); none when the file is missing.
// Lines that do not read are skipped.
std::vector<PluginApproval> read_plugin_approvals(const std::filesystem::path& profile_dir);
bool write_plugin_approvals(const std::filesystem::path& profile_dir, const std::vector<PluginApproval>& approvals);

// The answer for these bytes (Unasked: none, or no profile folder).
Consent plugin_consent(const std::filesystem::path& profile_dir, const Sha256Digest& hash);
// The same from answers already read (read_plugin_approvals).
Consent consent_in(const std::vector<PluginApproval>& approvals, const Sha256Digest& hash);
// Changes with every write_plugin_approvals in this process (any server's), so a holder of answers it read can tell
// when to read them again rather than reading the file each frame. A change from outside the client is seen when the
// holder reads again for another reason (the next zone-in's rules).
std::uint32_t plugin_approvals_generation();
// The answer for these bytes, in place of every earlier answer for this plugin name (another version's).
bool set_plugin_consent(const std::filesystem::path& profile_dir, std::string_view name, const Sha256Digest& hash,
                        bool allowed);
// Every answer for this plugin name, and for the bytes `hashes` (the plugin's files as the server has them now),
// turned to Denied, a hash not answered yet included: the player took the permission back. Approvals are by hash, so
// revoking by name alone would leave the same bytes allowed under the name the server sends them with next.
bool revoke_plugin_consent(const std::filesystem::path& profile_dir, std::string_view name,
                           const std::vector<Sha256Digest>& hashes = {});
// Every answer for this plugin name and for the bytes `hashes` forgotten: their next load asks again.
bool forget_plugin_consent(const std::filesystem::path& profile_dir, std::string_view name,
                           const std::vector<Sha256Digest>& hashes = {});

// A server plugin waiting for the player's answer: its name, file, size and hash.
struct ConsentFile {
    std::string name;
    std::filesystem::path file;
    std::uint64_t size = 0;
    Sha256Digest sha256{};
    // The player's own add-on (installed in plugins/) that the server's add-on policy requires, not a plugin the
    // server sent (docs/plugins.md "Opt-in add-on policies").
    bool required_addon = false;
};
// The question as the prompt shows it: the server's name, its folder, and the plugins asked about. `serial` changes
// whenever the question does.
struct ConsentPrompt {
    std::string server;
    std::filesystem::path profile_dir;
    std::vector<ConsentFile> files;
    std::uint32_t serial = 0;
};

// The prompt's text for the player (the C++ prompt and the UI packs show it): what a compiled plugin can do.
inline constexpr std::string_view kConsentWarning =
    "A compiled plugin is a program. It runs with full access to your computer: your files, your accounts and your "
    "network, not only the game. Allow it only if you trust this server.";

// The question's words for what it asks about (Hailstone's own; the original has no plugins): the server's plugins
// (kConsentWarning), the player's own add-ons the server requires (they come from the add-on's author, not the
// server), or both together. The title, the line before the list and the warning.
struct ConsentWording {
    std::string title, intro, warning;
};
ConsentWording consent_wording(const ConsentPrompt& prompt, std::string_view server);

} // namespace hailstone::plugin
