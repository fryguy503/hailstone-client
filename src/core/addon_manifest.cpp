#include "core/addon_manifest.h"

#include <charconv>

#include "core/addon_source.h"
#include "core/update_manifest.h"

namespace hailstone {

namespace {

constexpr std::size_t kMaxManifest = 64 * 1024;
constexpr std::size_t kMaxPackages = 32;
constexpr std::size_t kMaxText = 200;

std::vector<std::string_view> fields(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        std::size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
        if (j > i) out.push_back(line.substr(i, j - i));
        i = j;
    }
    return out;
}

// The rest of the line after its record's word, trimmed.
std::string_view rest_of(std::string_view line, std::string_view word) {
    std::string_view r = line.substr(line.find(word) + word.size());
    while (!r.empty() && (r.front() == ' ' || r.front() == '\t')) r.remove_prefix(1);
    while (!r.empty() && (r.back() == ' ' || r.back() == '\t')) r.remove_suffix(1);
    return r;
}

// Text to show the player: printable, no control characters, at most kMaxText bytes.
bool shown_text(std::string_view s) {
    if (s.empty() || s.size() > kMaxText) return false;
    for (char c : s)
        if (static_cast<unsigned char>(c) < ' ' || c == 0x7f) return false;
    return true;
}

bool platform_word(std::string_view s) {
    if (s.empty() || s.size() > 32) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    return true;
}

// A release asset's or a repository file's name: no folders, nothing a URL or a path could misread.
bool file_name(std::string_view s) {
    if (s.empty() || s.size() > 200 || s[0] == '.') return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
              c == '.'))
            return false;
    return true;
}

bool package_file(std::string_view s) {
    if (s.size() > 500) return false;
    if (s.substr(0, 8) == "https://") {
        for (char c : s)
            if (static_cast<unsigned char>(c) <= ' ' || c == 0x7f || c == '\\') return false;
        return addon_download_allowed(s);
    }
    return file_name(s);
}

bool web_address(std::string_view s) {
    if (s.size() > 500) return false;
    if (s.substr(0, 8) != "https://" && s.substr(0, 7) != "http://") return false;
    for (char c : s)
        if (static_cast<unsigned char>(c) <= ' ' || c == 0x7f) return false;
    return true;
}

bool cap_word(std::string_view s) {
    if (s.empty() || s.size() > 16) return false;
    for (char c : s)
        if (!(c >= 'a' && c <= 'z')) return false;
    return true;
}

std::nullopt_t fail(std::string* error, std::string why) {
    if (error) *error = std::move(why);
    return std::nullopt;
}

} // namespace

bool addon_name_ok(std::string_view name) {
    if (name.empty() || name.size() > 32) return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum && (i == 0 || (c != '-' && c != '_'))) return false;
    }
    return true;
}

const AddonPackage* addon_package_for(const AddonManifest& manifest, std::string_view platform) {
    for (const auto& p : manifest.packages)
        if (p.platform == platform) return &p;
    return nullptr;
}

std::optional<AddonManifest> parse_addon_manifest(std::string_view text, std::string* error) {
    if (text.size() > kMaxManifest) return fail(error, "the add-on's manifest is too large");
    // The signature, as the update manifest's: everything before its line, which ends the file.
    const std::string_view body = update_manifest_signed_part(text);
    std::optional<std::array<std::uint8_t, 64>> signature;
    if (body.size() != text.size()) {
        std::string_view sig_line = text.substr(body.size());
        if (!sig_line.empty() && sig_line.back() == '\n') sig_line.remove_suffix(1);
        if (!sig_line.empty() && sig_line.back() == '\r') sig_line.remove_suffix(1);
        if (sig_line.find('\n') != std::string_view::npos)
            return fail(error, "the add-on's manifest has lines after its signature");
        signature = hex_decode<64>(sig_line.substr(std::string_view("signature ").size()));
        if (!signature) return fail(error, "the add-on's signature is not readable");
    }

    AddonManifest m;
    bool header = false, have_name = false, have_kind = false, have_version = false, have_caps = false, have_autoload = false;
    std::optional<Ed25519PublicKey> key;
    std::size_t at = 0;
    int line_no = 0;
    while (at < body.size()) {
        std::size_t nl = body.find('\n', at);
        if (nl == std::string_view::npos) nl = body.size();
        std::string_view line = body.substr(at, nl - at);
        at = nl + 1;
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const auto f = fields(line);
        if (f.empty() || f[0][0] == '#') continue;
        const std::string where = "line " + std::to_string(line_no) + " of the add-on's manifest";
        if (!header) {
            if (f.size() != 2 || f[0] != "hailstone-addon") return fail(error, "this is not a Hailstone add-on manifest");
            if (f[1] != "1")
                return fail(error, "the add-on needs a newer Hailstone (its manifest is version " + std::string(f[1]) + ")");
            header = true;
        } else if (f[0] == "name") {
            if (f.size() != 2 || !addon_name_ok(f[1]) || have_name) return fail(error, where + " has a bad name");
            m.name = std::string(f[1]);
            have_name = true;
        } else if (f[0] == "kind") {
            if (f.size() != 2 || have_kind ||
                (f[1] != "script" && f[1] != "plugin" && f[1] != "tool" && f[1] != "upscaler" && f[1] != "ui"))
                return fail(error, where + ": the kind is script, plugin, tool, upscaler or ui");
            m.kind = f[1] == "script"     ? AddonKind::Script
                     : f[1] == "plugin"   ? AddonKind::Plugin
                     : f[1] == "tool"     ? AddonKind::Tool
                     : f[1] == "upscaler" ? AddonKind::Upscaler
                                           : AddonKind::Ui;
            have_kind = true;
        } else if (f[0] == "autoload") {
            if (f.size() != 2 || have_autoload || (f[1] != "0" && f[1] != "1"))
                return fail(error, where + " has a bad autoload switch");
            have_autoload = true;
            m.autoload = f[1] == "1";
        } else if (f[0] == "version" || f[0] == "min_client") {
            auto v = f.size() == 2 ? parse_version(f[1]) : std::nullopt;
            const bool version = f[0] == "version";
            if (!v || (version ? have_version : m.min_client.has_value()))
                return fail(error, where + " has a bad " + std::string(f[0]));
            if (version) {
                m.version = *v;
                m.version_text = std::string(f[1]);
                have_version = true;
            } else {
                m.min_client = *v;
                m.min_client_text = std::string(f[1]);
            }
        } else if (f[0] == "author" || f[0] == "description") {
            const std::string_view r = rest_of(line, f[0]);
            if (!shown_text(r)) return fail(error, where + " has a bad " + std::string(f[0]));
            (f[0] == "author" ? m.author : m.description) = std::string(r);
        } else if (f[0] == "caps") {
            if (have_caps) return fail(error, where + " repeats caps");
            have_caps = true;
            // "caps read,ui", "caps read, ui" or "caps none"
            std::string_view r = rest_of(line, "caps");
            while (!r.empty()) {
                std::size_t end = r.find_first_of(", \t");
                std::string_view w = r.substr(0, end);
                r = end == std::string_view::npos ? std::string_view() : r.substr(end + 1);
                if (w.empty()) continue;
                if (!cap_word(w)) return fail(error, where + " has a bad capability");
                if (w == "none") continue;
                bool seen = false;
                for (const auto& c : m.caps) seen = seen || c == w;
                if (!seen && m.caps.size() < 32) m.caps.emplace_back(w);
            }
        } else if (f[0] == "notes") {
            if (f.size() != 2 || !web_address(f[1])) return fail(error, where + " has a bad notes address");
            m.notes = std::string(f[1]);
        } else if (f[0] == "package") {
            if (f.size() != 5) return fail(error, where + " is not a package");
            AddonPackage p;
            p.platform = std::string(f[1]);
            auto [end, ec] = std::from_chars(f[2].data(), f[2].data() + f[2].size(), p.size);
            auto digest = parse_sha256(f[3]);
            if (!platform_word(f[1]) || ec != std::errc{} || end != f[2].data() + f[2].size() || p.size == 0 ||
                !digest || !package_file(f[4]))
                return fail(error, where + " is not a package");
            p.sha256 = *digest;
            p.file = std::string(f[4]);
            if (addon_package_for(m, p.platform)) return fail(error, where + " repeats a platform");
            if (m.packages.size() >= kMaxPackages) return fail(error, "the add-on's manifest has too many packages");
            m.packages.push_back(std::move(p));
        } else if (f[0] == "key") {
            if (f.size() != 2 || key) return fail(error, where + " has a bad key");
            key = hex_decode<32>(f[1]);
            if (!key) return fail(error, where + " has a bad key");
        }
        // Any other record is a later version's.
    }
    if (!header) return fail(error, "this is not a Hailstone add-on manifest");
    if (!have_name) return fail(error, "the add-on's manifest has no name");
    if (!have_kind) return fail(error, "the add-on's manifest has no kind");
    if (m.autoload && m.kind != AddonKind::Script) return fail(error, "autoload is only for managed Lua add-ons");
    if (!have_version) return fail(error, "the add-on's manifest has no version");
    if (!have_caps && m.kind != AddonKind::Ui)
        return fail(error, "the add-on's manifest does not say what it needs (caps)");
    if (m.packages.empty()) return fail(error, "the add-on's manifest has no package");
    for (const auto& p : m.packages) {
        if ((m.kind == AddonKind::Script || m.kind == AddonKind::Ui) && p.platform != "any")
            return fail(error, "a script or UI package is for any platform (package any ...)");
        if (m.kind == AddonKind::Plugin && p.platform == "any")
            return fail(error, "a plugin's packages are each for one platform (package linux-x64 ...)");
        if (m.kind == AddonKind::Tool && p.platform == "any")
            return fail(error, "a tool's packages are each for one platform (package linux-x64 ...)");
        if (m.kind == AddonKind::Upscaler && p.platform == "any")
            return fail(error, "an upscaler's packages are each for one platform (package windows-x64 ...)");
    }
    // A tool is a program of its own, and an upscaler sees only the frames it upscales: the client grants neither
    // anything to ask for.
    if (m.kind == AddonKind::Tool && !m.caps.empty()) return fail(error, "a tool asks for no capabilities (caps none)");
    if (m.kind == AddonKind::Upscaler && !m.caps.empty())
        return fail(error, "an upscaler asks for no capabilities (caps none)");
    if (m.kind == AddonKind::Ui && !m.caps.empty()) return fail(error, "a UI pack asks for no capabilities (caps none)");
    if (key && !signature) return fail(error, "the add-on's manifest has a key but no signature");
    if (signature && !key) return fail(error, "the add-on's manifest is signed but does not give its key");
    if (key) {
        if (!ed25519_verify(*signature, *key, body))
            return fail(error, "the add-on's signature does not match its key");
        m.key = key;
    }
    return m;
}

} // namespace hailstone
