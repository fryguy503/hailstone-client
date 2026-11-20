#include "plugin/ui_bridge.h"

#include <algorithm>
#include <cctype>

namespace hailstone::plugin {

namespace {
bool iequal(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
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
    return std::string(kind == Kind::Plugin ? "plugin:" : "script:") + std::string(name);
}

std::optional<std::pair<UiBridge::Kind, std::string>> UiBridge::parse_tag(std::string_view tag) {
    if (tag.starts_with("plugin:")) return std::pair{Kind::Plugin, std::string(tag.substr(7))};
    if (tag.starts_with("script:")) return std::pair{Kind::Script, std::string(tag.substr(7))};
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
    std::erase_if(folders_, [&](const auto& f) { return iequal(f.first, tag); });
}

} // namespace hailstone::plugin
