#include "plugin/ui_bridge.h"

#include <algorithm>
#include <cctype>

#include "hailstone_ui_contribution.h"

namespace hailstone::plugin {

namespace {
bool iequal(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

bool identifier(std::string_view value, std::size_t limit) {
    return !value.empty() && value.size() <= limit && std::ranges::all_of(value, [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '.' || c == ':' || c == '-';
    });
}

bool display_text(std::string_view value, std::size_t limit) {
    return value.size() <= limit && value.find_first_not_of(' ') != std::string_view::npos &&
           std::ranges::all_of(value, [](unsigned char c) { return c >= 32 && c != 127; });
}

std::optional<std::string_view> sdk_string(const char* data, std::size_t limit) {
    if (!data) return std::string_view{};
    std::size_t length = 0;
    while (length <= limit && data[length] != '\0') ++length;
    if (length > limit) return std::nullopt;
    return std::string_view{data, length};
}
} // namespace

UiValue UiValue::boolean(bool v) {
    UiValue x;
    x.kind = Kind::Bool;
    x.b = v;
    return x;
}

UiValue UiValue::integer(std::int64_t v) {
    UiValue x;
    x.kind = Kind::Int;
    x.i = v;
    return x;
}

UiValue UiValue::number(double v) {
    UiValue x;
    x.kind = Kind::Number;
    x.n = v;
    return x;
}

UiValue UiValue::string(std::string v) {
    UiValue x;
    x.kind = Kind::String;
    x.s = std::move(v);
    return x;
}

const UiValue* UiValue::field(std::string_view key) const {
    if (kind != Kind::Table) return nullptr;
    for (const auto& [k, v] : fields)
        if (k == key) return &v;
    return nullptr;
}

std::string UiBridge::owner_tag(Kind kind, std::string_view name) {
    std::string_view prefix = "plugin:";
    if (kind == Kind::Script) prefix = "script:";
    if (kind == Kind::Upscaler) prefix = "upscaler:";
    return std::string(prefix) + std::string(name);
}

std::optional<std::pair<UiBridge::Kind, std::string>> UiBridge::parse_tag(std::string_view tag) {
    if (tag.starts_with("plugin:")) return std::pair{Kind::Plugin, std::string(tag.substr(7))};
    if (tag.starts_with("script:")) return std::pair{Kind::Script, std::string(tag.substr(7))};
    if (tag.starts_with("upscaler:")) return std::pair{Kind::Upscaler, std::string(tag.substr(9))};
    return std::nullopt;
}

namespace {
// Whether an entry of `kind` named `name` is the owner `tag` names (a tag: owner_tag's form).
bool owned_by(UiBridge::Kind kind, std::string_view name, std::string_view tag) {
    auto t = UiBridge::parse_tag(tag);
    return t && t->first == kind && iequal(t->second, name);
}
} // namespace

std::string UiBridge::resolve(std::string_view plugin) const {
    auto known = [&](Kind kind, std::string_view name) {
        auto mine = [&](const auto& x) { return x.kind == kind && iequal(x.plugin, name); };
        const std::string tag = owner_tag(kind, name);
        return std::any_of(functions_.begin(), functions_.end(), mine) ||
               std::any_of(options_.begin(), options_.end(), mine) || std::any_of(menu_.begin(), menu_.end(), mine) ||
               std::any_of(folders_.begin(), folders_.end(), [&](const auto& f) { return iequal(f.first, tag); });
    };
    if (auto t = parse_tag(plugin)) return known(t->first, t->second) ? owner_tag(t->first, t->second) : std::string();
    if (known(Kind::Plugin, plugin)) return owner_tag(Kind::Plugin, plugin);
    if (known(Kind::Script, plugin)) return owner_tag(Kind::Script, plugin);
    return {};
}

namespace {
// How many entries of `list` the owner (kind, name) has.
template <class List>
std::size_t owned(const List& list, UiBridge::Kind kind, std::string_view plugin) {
    return static_cast<std::size_t>(std::count_if(list.begin(), list.end(), [&](const auto& x) {
        return x.kind == kind && iequal(x.plugin, plugin);
    }));
}
} // namespace

bool UiBridge::add_function(const std::string& plugin, const std::string& name, Fn fn, Kind kind) {
    remove_function(plugin, name, kind);
    if (owned(functions_, kind, plugin) >= kMaxFunctions) return false;
    functions_.push_back({plugin, name, std::move(fn), kind});
    return true;
}

void UiBridge::remove_function(std::string_view plugin, std::string_view name, Kind kind) {
    std::erase_if(functions_,
                  [&](const Function& f) { return f.kind == kind && iequal(f.plugin, plugin) && f.name == name; });
}

std::optional<UiValue> UiBridge::call(std::string_view plugin, std::string_view name,
                                      const std::vector<UiValue>& args) const {
    if (args.size() > kMaxCallArgs) return std::nullopt;
    const std::string owner = resolve(plugin);
    if (owner.empty()) return std::nullopt;
    for (const Function& f : functions_)
        if (owned_by(f.kind, f.plugin, owner) && f.name == name) {
            Fn fn = f.fn; // the call may change the list (a plugin removing its functions)
            return fn(args);
        }
    return std::nullopt;
}

bool UiBridge::has_plugin(std::string_view plugin) const { return !resolve(plugin).empty(); }

bool UiBridge::add_option(Option o) {
    for (Option& old : options_)
        if (old.kind == o.kind && iequal(old.plugin, o.plugin) && old.name == o.name) {
            old = std::move(o);
            return true;
        }
    if (owned(options_, o.kind, o.plugin) >= kMaxOptions) return false;
    options_.push_back(std::move(o));
    return true;
}

UiBridge::Option* UiBridge::find_option(std::string_view plugin, std::string_view name) {
    const std::string owner = resolve(plugin);
    if (owner.empty()) return nullptr;
    for (Option& o : options_)
        if (owned_by(o.kind, o.plugin, owner) && o.name == name) return &o;
    return nullptr;
}

bool UiBridge::add_menu(MenuEntry e) {
    for (MenuEntry& old : menu_)
        if (old.kind == e.kind && iequal(old.plugin, e.plugin) && old.label == e.label) {
            old.command = std::move(e.command);
            return true;
        }
    if (owned(menu_, e.kind, e.plugin) >= kMaxMenuEntries) return false;
    menu_.push_back(std::move(e));
    return true;
}

bool UiBridge::escape_target(std::string_view target) {
    return target == "setting:upscaler" || target == "setting:ui_name" || target == "setting:texture_pack";
}

bool UiBridge::set_contribution(Contribution entry) {
    if (entry.plugin.empty() || !identifier(entry.id, kMaxContributionIdBytes) ||
        !identifier(entry.window, kMaxContributionWindowBytes) ||
        (!entry.target.empty() && !identifier(entry.target, kMaxContributionTargetBytes))) return false;
    if (!entry.text.empty() && !display_text(entry.text, kMaxContributionTextBytes)) return false;
    if (entry.disabled) {
        if (entry.target.empty() || escape_target(entry.target)) return false;
        if (entry.text.empty()) entry.text = "This control is disabled.";
    }
    if (!entry.action_label.empty() || !entry.command.empty()) {
        if (entry.kind == Kind::Upscaler || !display_text(entry.action_label, kMaxContributionActionLabelBytes) ||
            !display_text(entry.command, kMaxContributionCommandBytes) ||
            !entry.command.starts_with('/') || entry.command.size() < 2 || entry.command[1] == ' ') return false;
    } else if (entry.text.empty()) {
        return false;
    }

    for (auto& old : contributions_)
        if (old.kind == entry.kind && iequal(old.plugin, entry.plugin) && old.id == entry.id) {
            old = std::move(entry);
            return true;
        }
    if (contributions_.size() >= kMaxTotalContributions ||
        owned(contributions_, entry.kind, entry.plugin) >= kMaxContributions) return false;
    contributions_.push_back(std::move(entry));
    return true;
}

bool UiBridge::set_contribution_from_sdk(std::string_view plugin, const HailstoneUiContribution* entry, Kind kind) {
    constexpr auto prefix_size = offsetof(HailstoneUiContribution, command) + sizeof(HailstoneUiContribution::command);
    if (!entry || entry->size < prefix_size ||
        (entry->flags & ~HAILSTONE_UI_CONTRIBUTION_DISABLED)) return false;
    Contribution contribution;
    contribution.plugin = plugin;
    contribution.kind = kind;
    contribution.disabled = (entry->flags & HAILSTONE_UI_CONTRIBUTION_DISABLED) != 0;
    struct Field { const char* source; std::size_t limit; std::string& destination; };
    const Field fields[] = {
        {entry->id, kMaxContributionIdBytes, contribution.id},
        {entry->window, kMaxContributionWindowBytes, contribution.window},
        {entry->target, kMaxContributionTargetBytes, contribution.target},
        {entry->text, kMaxContributionTextBytes, contribution.text},
        {entry->action_label, kMaxContributionActionLabelBytes, contribution.action_label},
        {entry->command, kMaxContributionCommandBytes, contribution.command},
    };
    for (const auto& field : fields) {
        auto value = sdk_string(field.source, field.limit);
        if (!value) return false;
        field.destination = *value;
    }
    return set_contribution(std::move(contribution));
}

bool UiBridge::remove_contribution(std::string_view plugin, std::string_view id, Kind kind) {
    return std::erase_if(contributions_, [&](const Contribution& entry) {
        return entry.kind == kind && iequal(entry.plugin, plugin) && entry.id == id;
    }) != 0;
}

bool UiBridge::remove_contribution_from_sdk(std::string_view plugin, const char* id, Kind kind) {
    const auto key = sdk_string(id, kMaxContributionIdBytes);
    return key && !key->empty() && remove_contribution(plugin, *key, kind);
}

void UiBridge::remove_contributions(std::string_view plugin, Kind kind) {
    std::erase_if(contributions_, [&](const Contribution& entry) {
        return entry.kind == kind && iequal(entry.plugin, plugin);
    });
}

const UiBridge::Contribution* UiBridge::find_contribution(std::string_view window, std::string_view owner,
                                                         std::string_view id) const {
    auto tag = parse_tag(owner);
    if (!tag) return nullptr;
    const auto match = std::ranges::find_if(contributions_, [&](const Contribution& entry) {
        return entry.window == window && entry.id == id && entry.kind == tag->first && iequal(entry.plugin, tag->second);
    });
    return match == contributions_.end() ? nullptr : &*match;
}

void UiBridge::set_folder(const std::string& plugin, std::filesystem::path folder, Kind kind) {
    const std::string tag = owner_tag(kind, plugin);
    for (auto& [p, f] : folders_)
        if (iequal(p, tag)) {
            f = std::move(folder);
            return;
        }
    folders_.emplace_back(tag, std::move(folder));
}

void UiBridge::remove(std::string_view plugin, Kind kind) {
    auto mine = [&](const auto& x) { return x.kind == kind && iequal(x.plugin, plugin); };
    const std::string tag = owner_tag(kind, plugin);
    std::erase_if(functions_, mine);
    std::erase_if(options_, mine);
    std::erase_if(menu_, mine);
    remove_contributions(plugin, kind);
    std::erase_if(folders_, [&](const auto& f) { return iequal(f.first, tag); });
}

} // namespace hailstone::plugin
