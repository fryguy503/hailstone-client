#pragma once

// A release's update manifest (docs/updater.md): hailstone-update.txt, an asset of every release, fetched from the
// update feed (GitHub's releases/latest/download/ link) to learn of a newer Hailstone. Text, one record a line:
//
//   hailstone-update 1
//   version 0.2.0-alpha
//   notes https://github.com/<owner>/<repo>/releases/tag/v0.2.0-alpha
//   package linux-x64 core 81234567 <sha256> hailstone-0.2.0-alpha-linux-x64-core.zip
//   package linux-x64 plugin-nms 230011 <sha256> hailstone-0.2.0-alpha-linux-x64-nms.zip plugins/nms
//   signature <Ed25519 signature, 128 hex digits>
//
// A package is one zip of a platform's component, its paths relative to the Hailstone folder; its file name is
// taken relative to the manifest's address (so the same release's asset). The optional last field is a folder the
// package is for: it is installed only when that folder is there (an optional plugin the player installed). The
// signature is the release key's over every byte before the signature line, which ends the file. Unknown records are
// skipped, so a later client can be sent more; a record it knows that does not parse refuses the whole manifest.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/ed25519.h"
#include "core/sha256.h"
#include "core/version.h"

namespace hailstone {

struct UpdatePackage {
    std::string platform;  // update_platform()'s names: "linux-x64", "windows-x64" ...
    std::string component; // "core" (the client), "browser", "plugin-<name>"
    std::uint64_t size = 0;
    Sha256Digest sha256{};
    std::string file;      // a file name, no folders
    std::string needs;     // a folder of the install it is for ("" always installed)
};

struct UpdateManifest {
    Version version;
    std::string version_text; // as written
    std::string notes;        // the release's page (http or https; "" none)
    std::vector<UpdatePackage> packages;
    std::string bastion_channel; // a signed downstream promotion record ("" for upstream releases)
    std::optional<Sha256Digest> bastion_runtime_sha256;
};

// The manifest in `text` when its signature is `key`'s and it parses; nullopt with *error why not.
std::optional<UpdateManifest> parse_update_manifest(std::string_view text, const Ed25519PublicKey& key,
                                                    std::string* error = nullptr);

// The part a signature covers: everything before the "signature " line, or the whole text without one.
std::string_view update_manifest_signed_part(std::string_view text);
// What follows the "signature " line ("" without one). Every reader refuses a manifest that goes on after it, and
// tools/hailstone_sign refuses to sign one rather than drop those lines.
std::string_view update_manifest_after_signature(std::string_view text);

// This build's platform as manifests name it ("linux-x64", "linux-arm64", "windows-x64", "windows-arm64",
// "macos-arm64", "macos-x64"; "" for any other).
std::string_view update_platform();

// The packages of `platform`.
std::vector<UpdatePackage> packages_for(const UpdateManifest& manifest, std::string_view platform);

} // namespace hailstone
