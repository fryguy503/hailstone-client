// An explicit local install through the same verification, unpacking, registry and rollback path as Add-ons.
// It never edits client/script autoload settings or starts code. Intended for local preview packaging.
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

#include "app/addon_install.h"
#include "app/addon_suggest.h"
#include "core/addon_manifest.h"
#include "core/addon_source.h"
#include "core/build_info.h"
#include "core/sha256.h"
#include "core/update_manifest.h"
#include "plugin/policy.h"

int main(int argc, char** argv) {
    using namespace hailstone;
    namespace fs = std::filesystem;
    std::map<std::string, std::string> args;
    for (int i = 1; i + 1 < argc; i += 2) args[argv[i]] = argv[i + 1];
    if (argc % 2 != 1 || !args.contains("--root") || !args.contains("--package") || !args.contains("--manifest")) {
        std::fputs("usage: hailstone_addon_install_local --root <install> --package <zip> --manifest <manifest> [--source owner/repo]\n", stderr);
        return 2;
    }
    auto fail = [](const std::string& message) { std::fprintf(stderr, "%s\n", message.c_str()); return 1; };
    const fs::path root = fs::absolute(args["--root"]);
    const fs::path package = fs::absolute(args["--package"]);
    const fs::path manifest_file = fs::absolute(args["--manifest"]);
    std::error_code ec;
    if (fs::file_size(manifest_file, ec) > 65536 || ec) return fail("Cannot read a manifest of at most 64 KiB.");
    std::ifstream input(manifest_file, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    std::string why;
    const auto manifest = parse_addon_manifest(text, &why);
    if (!manifest) return fail(why);
    const bool portable = manifest->kind == AddonKind::Script || manifest->kind == AddonKind::Ui;
    const auto* expected = addon_package_for(*manifest, portable ? "any" : update_platform());
    if (!expected || fs::file_size(package, ec) != expected->size || ec) return fail("The package size does not match its manifest.");
    if (manifest->min_client && compare_versions(*parse_version(release_version()), *manifest->min_client) < 0)
        return fail("The add-on needs a newer client.");
    Sha256 hash;
    std::ifstream bytes(package, std::ios::binary);
    std::array<char, 65536> buffer{};
    while (bytes.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || bytes.gcount() > 0)
        hash.update(std::string_view(buffer.data(), static_cast<std::size_t>(bytes.gcount())));
    if (!bytes.eof() || hash.finish() != expected->sha256) return fail("The package SHA-256 does not match its manifest.");
    const auto caps = plugin::parse_caps_strict([&] {
        std::string out;
        for (const auto& cap : manifest->caps) out += (out.empty() ? "" : ",") + cap;
        return out.empty() ? std::string("none") : out;
    }());
    if (!caps || ((manifest->kind == AddonKind::Script || manifest->kind == AddonKind::Plugin) && *caps == 0))
        return fail("The manifest names no usable capability or an unknown capability.");
    app::addons_at_start(root);
    const auto installed = app::read_installed_addons(root);
    const std::string source = args.contains("--source") ? args["--source"] : "local/managed-preview";
    if (!parse_addon_link(source)) return fail("The source must name an add-on repository.");
    if (const auto* old = app::find_addon(installed, manifest->name)) {
        if (app::addon_repo_key(old->source) != app::addon_repo_key(source)) return fail("The installed add-on belongs to another repository.");
        if (old->key && old->key != manifest->key) return fail("The installed add-on is pinned to another signing key.");
    } else
        if (const auto taken = app::addon_name_taken(root, installed, manifest->kind, manifest->name); !taken.empty()) return fail(taken);
    auto files = app::unpack_addon(root, *manifest, package, update_platform(), &why);
    if (!files) return fail(why);
    app::InstalledAddon record;
    record.name = manifest->name;
    record.kind = manifest->kind;
    record.source = source;
    record.version = manifest->version_text;
    record.caps = *caps;
    record.key = manifest->key;
    record.autoload = manifest->autoload;
    record.enabled = manifest->autoload;
    record.author = manifest->author;
    record.description = manifest->description;
    record.notes = manifest->notes;
    record.files = std::move(*files);
    if (!app::apply_addon(root, record, &why)) return fail(why);
    std::printf("Installed %s %s through Add-ons into %s\n", record.name.c_str(), record.version.c_str(), root.string().c_str());
    return 0;
}
