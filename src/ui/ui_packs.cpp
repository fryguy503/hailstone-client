#include "ui/ui_packs.h"

#include <algorithm>
#include <system_error>

#include "core/addon_manifest.h"
#include "core/config.h"
#include "core/log.h"

namespace hailstone::ui {

namespace fs = std::filesystem;

namespace {

bool contained(const fs::path& root, const fs::path& path) {
    std::error_code ec;
    const auto canonical_root = fs::weakly_canonical(root, ec);
    if (ec) return false;
    const auto canonical_path = fs::weakly_canonical(path, ec);
    if (ec) return false;
    const auto relative = canonical_path.lexically_relative(canonical_root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != ".." && *relative.begin() != ".";
}

bool pack_id(std::string_view id) {
    return !id.empty() && id != "." && id != ".." && id.find_first_of("/\\:") == std::string_view::npos &&
           std::none_of(id.begin(), id.end(), [](char c) {
               const auto value = static_cast<unsigned char>(c);
               return value < 32 || value == 127;
           });
}

std::string folded(std::string_view value) {
    std::string out(value);
    for (char& c : out) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return out;
}

} // namespace

std::vector<UiPack> list_ui_packs(const fs::path& root, std::vector<std::string>* diagnostics) {
    return list_ui_packs(root, {}, {}, diagnostics);
}

std::vector<UiPack> list_ui_packs(const fs::path& root, const fs::path& managed_root,
                                const std::vector<std::string>& installed_ui_addons,
                                std::vector<std::string>* diagnostics) {
    struct Candidate {
        UiPack pack;
        bool own_input = false, own_art = false;
    };
    std::vector<Candidate> candidates;
    auto reject = [&](std::string_view id, std::string_view reason) {
        const std::string message = "UI pack '" + std::string(id) + "': " + std::string(reason);
        log::warn("{}", message);
        if (diagnostics) diagnostics->push_back(message);
    };
    auto scan = [&](const fs::path& source, std::string_view addon) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(source, ec)) {
            if (!e.is_directory(ec)) continue;
            const auto local = e.path().filename().string();
            const auto id = addon.empty() ? local : "addon." + folded(addon) + '.' + folded(local);
            if (!pack_id(local) || (!addon.empty() && !addon_name_ok(local)) ||
                (addon.empty() && folded(local).starts_with("addon.")) ||
                !contained(source, e.path()) || !contained(e.path(), e.path() / "ui.ini")) {
                reject(id, "folder or manifest escapes the UI root");
                continue;
            }
            Config c;
            if (!c.load(e.path() / "ui.ini")) continue;
            Candidate candidate;
            auto& p = candidate.pack;
            p.id = id;
            p.name = c.get_string("name", p.id);
            p.description = c.get_string("description");
            p.input = c.get_string("input", "mouse");
            p.game_art = c.get_string("game_art", "icons") == "all";
            p.base = c.get_string("base");
            p.dir = e.path();
            p.local_id = local;
            p.addon = addon;
            p.source = addon.empty() ? "builtin" : "addon";
            p.runtime = c.get_string("runtime", "lua");
            if (p.runtime != "lua") {
                p.available = false;
                p.unavailable_reason = "Unsupported interface runtime: " + p.runtime;
            }
            candidate.own_input = c.get("input").has_value();
            candidate.own_art = c.get("game_art").has_value();
            candidates.push_back(std::move(candidate));
        }
    };
    scan(root, {});
    for (const auto& addon : installed_ui_addons) {
        const auto owner = managed_root / addon;
        const auto source = owner / "ui";
        if (managed_root.empty() || !addon_name_ok(addon) || !contained(managed_root, owner) || !contained(owner, source)) {
            reject(addon, "installed add-on root escapes its owner");
            continue;
        }
        scan(source, addon);
    }
    auto ambiguous = [&](std::string_view id) {
        return std::count_if(candidates.begin(), candidates.end(), [&](const Candidate& p) {
            return folded(p.pack.id) == folded(id);
        }) > 1;
    };
    std::vector<UiPack> out;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (ambiguous(candidates[index].pack.id)) {
            reject(candidates[index].pack.id, "duplicate interface ID");
            continue;
        }
        std::vector<std::size_t> chain{index};
        bool valid = true;
        while (!candidates[chain.back()].pack.base.empty()) {
            const auto& child = candidates[chain.back()].pack;
            std::string base = child.base;
            if (base.starts_with("builtin:")) base.erase(0, 8);
            else if (!child.addon.empty() && !base.starts_with("addon.")) {
                const std::string local = "addon." + folded(child.addon) + '.' + folded(base);
                if (std::any_of(candidates.begin(), candidates.end(), [&](const Candidate& p) { return p.pack.id == local; }))
                    base = local;
            }
            const auto parent = std::find_if(candidates.begin(), candidates.end(),
                                            [&](const Candidate& p) { return p.pack.id == base; });
            if (!pack_id(base) || parent == candidates.end() || ambiguous(base)) {
                reject(candidates[index].pack.id, "missing or invalid base");
                valid = false;
                break;
            }
            const auto next = static_cast<std::size_t>(parent - candidates.begin());
            if (std::find(chain.begin(), chain.end(), next) != chain.end() || chain.size() >= 32) {
                reject(candidates[index].pack.id, chain.size() >= 32 ? "base chain exceeds 32 packs" : "cyclic base chain");
                valid = false;
                break;
            }
            chain.push_back(next);
        }
        if (!valid) continue;
        UiPack pack = candidates[index].pack;
        bool input_found = candidates[index].own_input, art_found = candidates[index].own_art;
        for (std::size_t i = 1; i < chain.size(); ++i) {
            const auto& parent = candidates[chain[i]];
            pack.base_dirs.push_back(parent.pack.dir);
            pack.base_ids.push_back(parent.pack.id);
            if (!parent.pack.available) {
                pack.available = false;
                pack.unavailable_reason = "Base " + parent.pack.id + ": " + parent.pack.unavailable_reason;
            }
            if (!input_found && parent.own_input) { pack.input = parent.pack.input; input_found = true; }
            if (!art_found && parent.own_art) { pack.game_art = parent.pack.game_art; art_found = true; }
        }
        out.push_back(std::move(pack));
    }
    std::sort(out.begin(), out.end(), [](const UiPack& a, const UiPack& b) {
        return a.name == b.name ? a.id < b.id : a.name < b.name;
    });
    return out;
}

const UiPack* find_ui_pack(const std::vector<UiPack>& packs, std::string_view id) {
    for (std::string_view want : {id, std::string_view("modern")})
        for (const auto& p : packs)
            if (p.id == want && p.available) return &p;
    const auto first = std::find_if(packs.begin(), packs.end(), [](const UiPack& p) { return p.available; });
    return first == packs.end() ? nullptr : &*first;
}

const UiPack* find_ui_pack_exact(const std::vector<UiPack>& packs, std::string_view id) {
    for (const auto& pack : packs) if (pack.id == id) return &pack;
    return nullptr;
}

fs::path plugin_ui_folder(const fs::path& plugin, const UiPack& pack) {
    const auto root = plugin / "ui";
    auto existing = [&](const fs::path& path) {
        std::error_code ec;
        return contained(plugin, path) && fs::is_directory(path, ec);
    };
    const auto own = root / pack.id;
    if (existing(own)) return own;
    for (std::size_t i = 0; i < pack.base_dirs.size(); ++i) {
        const auto path = root / (i < pack.base_ids.size() ? fs::path(pack.base_ids[i]) : pack.base_dirs[i].filename());
        if (existing(path)) return path;
    }
    return contained(plugin, own) ? own : fs::path{};
}

} // namespace hailstone::ui
