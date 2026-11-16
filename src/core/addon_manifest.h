#pragma once

// An add-on's manifest (docs/addons.md): hailstone-addon.txt in a GitHub repository or as an asset of its releases,
// which the add-on manager fetches to install a script, a compiled plugin or a tool and to keep it up to date. A Hailstone
// feature with no original counterpart. Text, one record a line, like the update manifest (core/update_manifest.h):
//
//   hailstone-addon 1
//   name autoloot
//   kind script                      script | plugin | tool
//   version 1.3.0
//   min_client 0.2.0                 optional: the oldest Hailstone it works with
//   author Someone                   optional
//   description Loots what you tell it to.   optional, the rest of the line
//   caps read,events,actions,ui      what it needs (docs/plugins.md "Capabilities"); a ceiling on what it gets; a
//                                    tool's are none (it is a program of its own, not run by the client)
//   notes https://...                optional: its release notes
//   package any 41233 <sha256> autoloot-1.3.0.zip
//   key <Ed25519 public key, 64 hex digits>          optional
//   signature <Ed25519 signature, 128 hex digits>    with a key: over every byte before this line, which ends the file
//
// A script has one package, for "any" platform; a plugin or a tool one for each platform it is built for
// (update_platform()'s names). A package's file is a name beside the manifest's address (the same release's asset, or the same branch's
// file) or an https address on GitHub (core/addon_source.h). Unknown records are skipped, for later versions; a
// record this knows that does not parse refuses the whole manifest. Nothing here is trusted: the manifest comes from
// whoever controls the repository.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/ed25519.h"
#include "core/sha256.h"
#include "core/version.h"

namespace hailstone {

// A tool is a program of its own with its files (Hailstone Texture Studio), in tools/<name>/: the client starts it
// when the player asks, and never loads or runs anything of it itself.
enum class AddonKind : std::uint8_t { Script, Plugin, Tool };

struct AddonPackage {
    std::string platform; // "any" (a script's), or "linux-x64", "windows-x64" ... (a plugin's or a tool's)
    std::uint64_t size = 0;
    Sha256Digest sha256{};
    std::string file;     // a file name beside the manifest, or an https address on GitHub
};

struct AddonManifest {
    std::string name;      // addon_name_ok: its folder, scripts/<name>/, plugins/<name>/ or tools/<name>/
    AddonKind kind = AddonKind::Script;
    bool autoload = false; // a managed Lua add-on: installed activation is controlled by the player
    Version version;
    std::string version_text;
    std::optional<Version> min_client;
    std::string min_client_text;
    std::string author;
    std::string description;
    std::vector<std::string> caps; // the words as written, lower case (the caller checks they are capabilities); a
                                   // tool's are empty
    std::string notes;             // http(s), "" none
    std::vector<AddonPackage> packages;
    std::optional<Ed25519PublicKey> key; // set only when the signature checked against it
};

// The manifest in `text`; nullopt with *error why not. A manifest with a key must carry a signature that matches it,
// and one with a signature must carry the key.
std::optional<AddonManifest> parse_addon_manifest(std::string_view text, std::string* error = nullptr);

// Whether `name` can be an add-on's: 1 to 32 letters, digits, '-' and '_', starting with a letter or digit.
bool addon_name_ok(std::string_view name);

// The package for `platform` ("any" for a script), or null.
const AddonPackage* addon_package_for(const AddonManifest& manifest, std::string_view platform);

} // namespace hailstone
