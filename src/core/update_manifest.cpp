#include "core/update_manifest.h"

#include <charconv>

namespace hailstone {

namespace {

constexpr std::size_t kMaxManifest = 64 * 1024;
constexpr std::size_t kMaxPackages = 256;
constexpr std::uint64_t kMaxSetupBytes = 1024 * 1024 * 1024;
constexpr std::string_view kSignature = "signature ";

std::vector<std::string_view> fields(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') ++i;
        std::size_t j = i;
        while (j < line.size() && line[j] != ' ') ++j;
        if (j > i) out.push_back(line.substr(i, j - i));
        i = j;
    }
    return out;
}

bool word(std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    return true;
}

// A release asset's name: no folders, nothing a URL or a path could misread.
bool file_name(std::string_view s) {
    if (s.empty() || s.size() > 200 || s[0] == '.') return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
              c == '.'))
            return false;
    return true;
}

bool setup_file_name(std::string_view file, const std::string& version) {
    const std::string prefix = "Hailstone-" + version + "-";
    constexpr std::string_view suffix = "-Windows-x64-Setup.exe";
    constexpr std::size_t commit_length = 8;
    if (!file_name(file) || !file.starts_with(prefix) || !file.ends_with(suffix) ||
        file.size() != prefix.size() + commit_length + suffix.size())
        return false;
    const auto commit_text = file.substr(prefix.size(), commit_length);
    const auto commit = hex_decode<commit_length / 2>(commit_text);
    return commit && hex_encode(*commit) == commit_text;
}

// A relative folder of the install ("plugins/nms"): '/'-separated plain names, no "." or "..".
bool folder(std::string_view s) {
    if (s.empty() || s.size() > 200 || s.front() == '/' || s.back() == '/') return false;
    std::size_t start = 0;
    while (start <= s.size()) {
        std::size_t end = s.find('/', start);
        if (end == std::string_view::npos) end = s.size();
        std::string_view part = s.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        for (char c : part)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                  c == '_' || c == '.'))
                return false;
        start = end + 1;
    }
    return true;
}

bool web_address(std::string_view s) {
    if (s.size() > 500) return false;
    if (s.substr(0, 8) != "https://" && s.substr(0, 7) != "http://") return false;
    for (char c : s)
        if (static_cast<unsigned char>(c) <= ' ' || c == 0x7f) return false;
    return true;
}

std::nullopt_t fail(std::string* error, std::string why) {
    if (error) *error = std::move(why);
    return std::nullopt;
}

} // namespace

std::string_view update_manifest_signed_part(std::string_view text) {
    std::size_t at = 0;
    while (at < text.size()) {
        if (text.substr(at, kSignature.size()) == kSignature) return text.substr(0, at);
        std::size_t nl = text.find('\n', at);
        if (nl == std::string_view::npos) break;
        at = nl + 1;
    }
    return text;
}

std::string_view update_manifest_after_signature(std::string_view text) {
    const std::string_view rest = text.substr(update_manifest_signed_part(text).size());
    const std::size_t nl = rest.find('\n');
    return nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
}

std::optional<UpdateManifest> parse_update_manifest(std::string_view text, const Ed25519PublicKey& key,
                                                    std::string* error) {
    if (text.size() > kMaxManifest) return fail(error, "the update manifest is too large");
    const std::string_view body = update_manifest_signed_part(text);
    if (body.size() == text.size()) return fail(error, "the update manifest is not signed");
    std::string_view sig_line = text.substr(body.size());
    if (!sig_line.empty() && sig_line.back() == '\n') sig_line.remove_suffix(1);
    if (!sig_line.empty() && sig_line.back() == '\r') sig_line.remove_suffix(1);
    if (sig_line.find('\n') != std::string_view::npos)
        return fail(error, "the update manifest has lines after its signature");
    auto signature = hex_decode<64>(sig_line.substr(kSignature.size()));
    if (!signature) return fail(error, "the update manifest's signature is not readable");
    if (!ed25519_verify(*signature, key, body))
        return fail(error, "the update manifest's signature does not match the release key");

    UpdateManifest m;
    bool header = false, version = false;
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
        const std::string where = "line " + std::to_string(line_no) + " of the update manifest";
        if (!header) {
            if (f.size() != 2 || f[0] != "hailstone-update")
                return fail(error, "this is not an update manifest");
            if (f[1] != "1")
                return fail(error, "the update manifest is a newer kind (" + std::string(f[1]) +
                                       "): download the new version by hand");
            header = true;
        } else if (f[0] == "version") {
            auto v = f.size() == 2 ? parse_version(f[1]) : std::nullopt;
            if (!v || version) return fail(error, where + " has a bad version");
            m.version = *v;
            m.version_text = std::string(f[1]);
            version = true;
        } else if (f[0] == "notes") {
            if (f.size() != 2 || !web_address(f[1])) return fail(error, where + " has a bad notes address");
            m.notes = std::string(f[1]);
        } else if (f[0] == "bastion-channel") {
            if (f.size() != 2 || (f[1] != "preview" && f[1] != "stable") || !m.bastion_channel.empty())
                return fail(error, where + " has a bad release channel");
            m.bastion_channel = f[1];
        } else if (f[0] == "bastion-runtime-sha256") {
            const auto digest = f.size() == 2 ? parse_sha256(f[1]) : std::nullopt;
            if (!digest || m.bastion_runtime_sha256)
                return fail(error, where + " has a bad runtime digest");
            m.bastion_runtime_sha256 = digest;
        } else if (f[0] == "bastion-setup") {
            if (f.size() != 4 || m.bastion_setup) return fail(error, where + " has a bad Setup record");
            BastionSetupPackage setup;
            const auto [end, ec] = std::from_chars(f[1].data(), f[1].data() + f[1].size(), setup.size);
            const auto digest = parse_sha256(f[2]);
            if (ec != std::errc{} || end != f[1].data() + f[1].size() || setup.size == 0 || setup.size > kMaxSetupBytes ||
                !digest || to_hex(*digest) != f[2])
                return fail(error, where + " has a bad Setup size or digest");
            setup.sha256 = *digest;
            setup.file = f[3];
            m.bastion_setup = std::move(setup);
        } else if (f[0] == "package") {
            if (f.size() != 6 && f.size() != 7) return fail(error, where + " is not a package");
            UpdatePackage p;
            p.platform = std::string(f[1]);
            p.component = std::string(f[2]);
            auto [end, ec] = std::from_chars(f[3].data(), f[3].data() + f[3].size(), p.size);
            auto digest = parse_sha256(f[4]);
            if (!word(f[1]) || !word(f[2]) || ec != std::errc{} || end != f[3].data() + f[3].size() || p.size == 0 ||
                !digest || !file_name(f[5]) || (f.size() == 7 && !folder(f[6])))
                return fail(error, where + " is not a package");
            p.sha256 = *digest;
            p.file = std::string(f[5]);
            if (f.size() == 7) p.needs = std::string(f[6]);
            for (const auto& q : m.packages)
                if (q.platform == p.platform && q.component == p.component)
                    return fail(error, where + " repeats a package");
            if (m.packages.size() >= kMaxPackages) return fail(error, "the update manifest has too many packages");
            m.packages.push_back(std::move(p));
        }
        // Any other record is a later client's.
    }
    if (!header) return fail(error, "this is not an update manifest");
    if (!version) return fail(error, "the update manifest has no version");
    if (m.bastion_setup && !setup_file_name(m.bastion_setup->file, m.version_text))
        return fail(error, "the Setup filename does not match the release version");
    return m;
}

std::string_view update_platform() {
#if defined(_WIN32)
#  if defined(_M_ARM64) || defined(__aarch64__)
    return "windows-arm64";
#  elif defined(_M_X64) || defined(__x86_64__)
    return "windows-x64";
#  else
    return "";
#  endif
#elif defined(__APPLE__)
#  if defined(__aarch64__)
    return "macos-arm64";
#  elif defined(__x86_64__)
    return "macos-x64";
#  else
    return "";
#  endif
#elif defined(__linux__)
#  if defined(__aarch64__)
    return "linux-arm64";
#  elif defined(__x86_64__)
    return "linux-x64";
#  else
    return "";
#  endif
#else
    return "";
#endif
}

std::vector<UpdatePackage> packages_for(const UpdateManifest& manifest, std::string_view platform) {
    std::vector<UpdatePackage> out;
    for (const auto& p : manifest.packages)
        if (p.platform == platform) out.push_back(p);
    return out;
}

} // namespace hailstone
