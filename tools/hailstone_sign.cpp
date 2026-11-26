// hailstone_sign: the release key and the update manifest's signature (docs/updater.md); also an add-on author's key
// and their add-on manifest's (hailstone-addon.txt, docs/addons.md), and the add-on index's
// (hailstone-addon-index.txt, core/addon_index.h, signed with the release key), told apart by the file's first line.
//
//   hailstone_sign keygen <secret-key-file>            makes a release key; prints its public half (update-key.pub)
//   hailstone_sign public <secret-key-file>            prints the public half of a key
//   hailstone_sign sign <secret-key-file> <manifest>   signs the manifest in place (an old signature is replaced; an
//                                                      add-on's gets the key's line too; lines after an old
//                                                      signature stop it)
//   hailstone_sign verify <public-key> <manifest>      checks it and lists what it holds; the key is hex or a file
//   hailstone_sign try <public-key> <feed> <folder> <version> [<platform>]
//                                                      a release's dry run: the client's updater, as version, takes
//                                                      the feed's update into folder (a copy of an install)
//
// The secret key file is 128 hex digits (Monocypher's 64-byte Ed25519 secret key). Keep it out of the repository
// and every build machine but the one that signs; whoever has it can make every Hailstone install run their code.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include "app/addon_install.h"
#include "app/update_install.h"
#include "app/updater.h"
#include "core/addon_index.h"
#include "core/addon_manifest.h"
#include "core/atomic_file.h"
#include "core/ed25519.h"
#include "core/update_manifest.h"
#include "platform/http_get.h"
#include "platform/private_file.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

int usage() {
    std::fputs("usage: hailstone_sign keygen <secret-key-file>\n"
               "       hailstone_sign public <secret-key-file>\n"
               "       hailstone_sign sign <secret-key-file> <manifest>\n"
               "       hailstone_sign verify <public-key-hex-or-file> <manifest>\n"
               "       hailstone_sign try <public-key-hex-or-file> <feed-url> <folder> <version> [<platform>]\n",
               stderr);
    return 2;
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

std::optional<Ed25519SecretKey> read_secret(const fs::path& path) {
    auto text = read_text_file(path);
    if (!text) {
        std::fprintf(stderr, "cannot read %s\n", path.string().c_str());
        return std::nullopt;
    }
    auto key = hex_decode<64>(trimmed(*text));
    if (!key) std::fprintf(stderr, "%s is not a secret key (128 hex digits)\n", path.string().c_str());
    return key;
}

Ed25519PublicKey public_of(const Ed25519SecretKey& secret) {
    Ed25519PublicKey pub{};
    std::copy(secret.begin() + 32, secret.end(), pub.begin()); // Monocypher keeps the public half in the secret
    return pub;
}

int keygen(const fs::path& path) {
    std::error_code ec;
    if (fs::exists(path, ec)) {
        std::fprintf(stderr, "%s is there already: not overwritten\n", path.string().c_str());
        return 1;
    }
    // std::random_device is the system's random source on the platforms Hailstone is released from (getrandom on
    // Linux, RtlGenRandom with MSVC, arc4random on macOS).
    std::random_device rd;
    std::array<std::uint8_t, 32> seed{};
    for (std::size_t i = 0; i < seed.size(); i += 4) {
        const std::uint32_t v = rd();
        for (std::size_t j = 0; j < 4; ++j) seed[i + j] = static_cast<std::uint8_t>(v >> (8 * j));
    }
    Ed25519SecretKey secret{};
    Ed25519PublicKey pub{};
    ed25519_key_pair(seed, secret, pub);
    seed.fill(0);
    // Made owner-only from the start (0600 on POSIX), not chmodded after the umask's mode was already on it.
    if (!platform::write_private_file(path, hex_encode(secret) + "\n")) {
        std::fprintf(stderr, "cannot write %s\n", path.string().c_str());
        return 1;
    }
    std::printf("%s\n", hex_encode(pub).c_str());
    std::fprintf(stderr, "The secret key is in %s. For Hailstone's own releases the line above goes in update-key.pub; "
                         "an add-on's manifest gets it when signed.\n", path.string().c_str());
    return 0;
}

int sign(const fs::path& key_path, const fs::path& manifest) {
    auto secret = read_secret(key_path);
    if (!secret) return 1;
    auto text = read_text_file(manifest);
    if (!text) {
        std::fprintf(stderr, "cannot read %s\n", manifest.string().c_str());
        return 1;
    }
    // The old signature line is replaced; anything after it (a line added below it by hand) would be dropped
    // unseen, so it stops the signing instead.
    if (update_manifest_after_signature(*text).find_first_not_of(" \t\r\n") != std::string_view::npos) {
        std::fprintf(stderr, "%s: there are lines after its signature line; move them above it (or remove the old "
                             "signature line) and sign again\n", manifest.string().c_str());
        return 1;
    }
    std::string body(update_manifest_signed_part(*text));
    if (!body.empty() && body.back() != '\n') body += '\n';
    const bool addon = body.starts_with("hailstone-addon ");
    const bool index = body.starts_with("hailstone-addon-index ");
    if (addon) {
        // An add-on's manifest carries the key it is signed with: its line is replaced with this key's.
        std::istringstream in(body);
        std::string kept;
        for (std::string line; std::getline(in, line);)
            if (!line.starts_with("key ")) kept += line + "\n";
        body = kept + "key " + hex_encode(public_of(*secret)) + "\n";
    }
    if (index) {
        // The serial the client holds a fetched index to (core/addon_index.h addon_index_older): the time it is
        // signed, and always past the one it had, so a clock set back never makes a newer index look older.
        std::uint64_t serial = 0, old = 0;
        std::istringstream in(body);
        std::string kept, header;
        for (std::string line; std::getline(in, line);) {
            if (header.empty()) {
                header = line + "\n";
                continue;
            }
            if (line.starts_with("serial ")) {
                std::from_chars(line.data() + 7, line.data() + line.size(), old);
                continue;
            }
            kept += line + "\n";
        }
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch()).count();
        serial = std::max<std::uint64_t>(old + 1, now > 0 ? static_cast<std::uint64_t>(now) : 0);
        body = header + "serial " + std::to_string(serial) + "\n" + kept;
    }
    const std::string out = body + "signature " + hex_encode(ed25519_sign(*secret, body)) + "\n";
    std::string error;
    if (addon   ? !parse_addon_manifest(out, &error)
        : index ? !parse_addon_index(out, public_of(*secret), &error)
                : !parse_update_manifest(out, public_of(*secret), &error)) {
        std::fprintf(stderr, "%s: %s\n", manifest.string().c_str(), error.c_str());
        return 1;
    }
    if (!write_file_atomic(manifest, out)) return 1;
    std::printf("signed %s\n", manifest.string().c_str());
    return 0;
}

int verify(const std::string& key_arg, const fs::path& manifest) {
    std::string hex = key_arg;
    if (auto file = read_text_file(key_arg)) hex = trimmed(*file);
    auto key = hex_decode<32>(hex);
    if (!key) {
        std::fprintf(stderr, "%s is not a public key (64 hex digits)\n", key_arg.c_str());
        return 1;
    }
    auto text = read_text_file(manifest);
    if (!text) {
        std::fprintf(stderr, "cannot read %s\n", manifest.string().c_str());
        return 1;
    }
    std::string error;
    if (text->starts_with("hailstone-addon ")) {
        auto a = parse_addon_manifest(*text, &error);
        if (a && (!a->key || *a->key != *key)) error = a->key ? "signed with another key" : "not signed";
        if (!a || !error.empty()) {
            std::fprintf(stderr, "%s: %s\n", manifest.string().c_str(), error.c_str());
            return 1;
        }
        const std::string kind(app::addon_kind_word(a->kind));
        std::printf("add-on %s %s (%s), caps", a->name.c_str(), a->version_text.c_str(), kind.c_str());
        for (const auto& c : a->caps) std::printf(" %s", c.c_str());
        if (a->caps.empty()) std::printf(" none");
        std::printf("\n");
        for (const auto& p : a->packages)
            std::printf("  %s: %s, %llu bytes\n", p.platform.c_str(), p.file.c_str(),
                        static_cast<unsigned long long>(p.size));
        return 0;
    }
    if (text->starts_with("hailstone-addon-index ")) {
        auto index = parse_addon_index(*text, *key, &error);
        if (!index) {
            std::fprintf(stderr, "%s: %s\n", manifest.string().c_str(), error.c_str());
            return 1;
        }
        if (index->serial) std::printf("serial %llu\n", static_cast<unsigned long long>(*index->serial));
        else std::printf("no serial (signed by an older hailstone_sign)\n");
        for (const auto& a : index->addons) {
            std::printf("add-on %s from %s, key %s\n", a.name.c_str(), a.source.c_str(), hex_encode(a.key).c_str());
            for (const auto& w : index->worlds)
                if (w.addon == a.name)
                    std::printf("  world \"%s\" on %s\n", w.world.c_str(),
                                w.login.empty() ? "any login server" : w.login.c_str());
        }
        return 0;
    }
    auto m = parse_update_manifest(*text, *key, &error);
    if (!m) {
        std::fprintf(stderr, "%s: %s\n", manifest.string().c_str(), error.c_str());
        return 1;
    }
    std::printf("version %s%s%s\n", m->version_text.c_str(), m->notes.empty() ? "" : ", notes ", m->notes.c_str());
    for (const auto& p : m->packages)
        std::printf("  %s %s: %s, %llu bytes%s%s\n", p.platform.c_str(), p.component.c_str(), p.file.c_str(),
                    static_cast<unsigned long long>(p.size), p.needs.empty() ? "" : ", for ", p.needs.c_str());
    return 0;
}

int try_update(const std::string& key_arg, const std::string& feed, const fs::path& folder, const std::string& version,
               const std::string& platform) {
    std::string hex = key_arg;
    if (auto file = read_text_file(key_arg)) hex = trimmed(*file);
    auto key = hex_decode<32>(hex);
    if (!key) {
        std::fprintf(stderr, "%s is not a public key (64 hex digits)\n", key_arg.c_str());
        return 1;
    }
    std::string why;
    if (!platform::http_available(&why)) {
        std::fprintf(stderr, "no HTTP: %s\n", why.c_str());
        return 1;
    }
    app::UpdaterEnv env;
    env.install_dir = folder;
    env.feed = feed;
    env.version = version;
    env.platform = platform;
    env.key = *key;
    env.fetch = [](const std::string& url, const std::function<bool(std::span<const std::byte>)>& sink,
                   const std::function<bool(std::uint64_t, std::uint64_t)>& progress,
                   const std::function<bool()>& cancelled) {
        platform::HttpGetOptions options;
        options.user_agent = "hailstone_sign";
        options.cancelled = cancelled;
        return platform::http_get(url, options, sink, progress);
    };
    app::Updater updater(env);
    updater.check();
    updater.wait();
    // As the login screen's Download: the client downloads only when the player asks (update.download is off).
    if (updater.status().state == app::UpdateState::Available) {
        updater.download();
        updater.wait();
    }
    const app::UpdateStatus s = updater.status();
    if (s.state != app::UpdateState::Ready) {
        std::fprintf(stderr, "not ready (state %d): %s\n", static_cast<int>(s.state), s.error.c_str());
        return 1;
    }
    std::printf("downloaded %s (%llu bytes)\n", s.version.c_str(), static_cast<unsigned long long>(s.total));
    const app::InstallResult r = app::install_staged_update(folder, version);
    if (!r.ok) {
        std::fprintf(stderr, "not installed: %s\n", r.error.c_str());
        return 1;
    }
    app::update_at_start(folder, r.version);
    app::update_started_fine(folder, r.version);
    std::printf("installed %s into %s\n", r.version.c_str(), folder.string().c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    if (cmd == "keygen" && argc == 3) return keygen(argv[2]);
    if (cmd == "public" && argc == 3) {
        auto secret = read_secret(argv[2]);
        if (!secret) return 1;
        std::printf("%s\n", hex_encode(public_of(*secret)).c_str());
        return 0;
    }
    if (cmd == "sign" && argc == 4) return sign(argv[2], argv[3]);
    if (cmd == "verify" && argc == 4) return verify(argv[2], argv[3]);
    if (cmd == "try" && (argc == 6 || argc == 7))
        return try_update(argv[2], argv[3], argv[4], argv[5], argc == 7 ? argv[6] : std::string(update_platform()));
    return usage();
}
