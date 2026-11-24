#include "plugin/hooks.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <map>
#include <variant>

#include "core/log.h"
#include "game/slash_commands.h"

namespace hailstone::plugin {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// The hook kinds, one list each.
enum Kind : std::size_t { kPacket, kChat, kLine, kCommand, kPulse, kState, kZone, kSpawn, kImgui, kKinds };

struct Entry {
    Hooks::Owner owner = 0;
    bool alive = true;
    Stream stream = Stream::Zone;
    Direction dir = Direction::In;
    std::uint16_t opcode = 0;
    std::string name; // commands: "/name", lower case
    std::uint32_t required = 0;
    std::variant<std::monostate, Hooks::PacketFn, std::function<Flow(ChatLine&)>, std::function<Flow(std::string&)>,
                 std::function<void(std::string_view)>, std::function<void()>, std::function<void(int)>,
                 std::function<void(ZonePhase)>, std::function<void(std::uint32_t, bool)>>
        fn;
};

using EntryPtr = std::shared_ptr<Entry>;

// Packets a plugin sends from its own packet hook go through the hooks again; this deep, they stop.
constexpr int kMaxPacketDepth = 8;
// Lines a command (or a line filter) runs from inside its own run nest on the native stack; this deep, they stop. A
// script's line passes the hooks twice (mq::Commands::filter sends it on again), and /docommand and the like stop
// themselves sooner (mq::kMaxNestedCommands), so only a plugin's own command that runs itself gets here.
constexpr int kMaxCommandDepth = 64;

} // namespace

struct Hooks::Impl {
    std::vector<EntryPtr> lists[kKinds];
    std::map<Owner, std::string> owners{{kClient, "client"}};
    std::vector<Owner> guests; // plugins' and scripts' owners (new_guest)
    Owner next_owner = 1;
    int depth = 0; // raising, nested; dead entries are swept when it returns to 0
    int packet_depth = 0;
    int command_depth = 0;
    bool dirty = false;
    std::uint32_t command_catalog_serial = 1;

    void add(Kind k, EntryPtr e) { lists[k].push_back(std::move(e)); }
    void command_catalog_changed() {
        if (++command_catalog_serial == 0) ++command_catalog_serial;
    }

    void sweep() {
        if (depth || !dirty) return;
        for (auto& list : lists)
            list.erase(std::remove_if(list.begin(), list.end(), [](const EntryPtr& e) { return !e->alive; }), list.end());
        dirty = false;
    }

    // Calls fn(entry) for each live entry of kind k (a snapshot: hooks added meanwhile wait for the next event);
    // stops when fn returns true.
    template <typename F> bool each(Kind k, F&& fn) {
        ++depth;
        std::vector<EntryPtr> snapshot = lists[k];
        bool stopped = false;
        for (const EntryPtr& e : snapshot) {
            if (!e->alive) continue;
            try {
                if (fn(*e)) {
                    stopped = true;
                    break;
                }
            } catch (const std::exception& ex) {
                log::warn("plugin hook of {} failed: {}", owners[e->owner], ex.what());
            } catch (...) {
                log::warn("plugin hook of {} failed", owners[e->owner]);
            }
        }
        --depth;
        sweep();
        return stopped;
    }
};

Hooks::Hooks() : impl_(std::make_unique<Impl>()) {}
Hooks::~Hooks() = default;

Hooks::Owner Hooks::new_owner(std::string name) {
    Owner o = impl_->next_owner++;
    impl_->owners[o] = std::move(name);
    return o;
}

Hooks::Owner Hooks::new_guest(std::string name) {
    Owner o = new_owner(std::move(name));
    impl_->guests.push_back(o);
    return o;
}

void Hooks::remove(Owner owner) {
    const auto& commands = impl_->lists[kCommand];
    if (std::any_of(commands.begin(), commands.end(), [owner](const auto& entry) {
        return entry->alive && entry->owner == owner;
    })) impl_->command_catalog_changed();
    for (auto& list : impl_->lists)
        for (auto& e : list)
            if (e->owner == owner) e->alive = false;
    impl_->dirty = true;
    if (owner != kClient) impl_->owners.erase(owner);
    std::erase(impl_->guests, owner);
    impl_->sweep();
}

void Hooks::remove_command(Owner owner, std::string_view name) {
    std::string n = lower(name);
    if (!n.empty() && n.front() != '/') n.insert(n.begin(), '/');
    bool removed = false;
    for (auto& e : impl_->lists[kCommand]) {
        if (!e->alive || e->owner != owner || e->name != n) continue;
        e->alive = false;
        removed = true;
    }
    if (removed) impl_->command_catalog_changed();
    impl_->dirty = true;
    impl_->sweep();
}

const std::string& Hooks::owner_name(Owner owner) const {
    static const std::string kNone = "?";
    auto it = impl_->owners.find(owner);
    return it == impl_->owners.end() ? kNone : it->second;
}

void Hooks::on_packet(Owner owner, Stream stream, Direction dir, std::uint16_t opcode, PacketFn fn) {
    auto e = std::make_shared<Entry>();
    e->owner = owner;
    e->stream = stream;
    e->dir = dir;
    e->opcode = opcode;
    e->fn.emplace<PacketFn>(std::move(fn));
    impl_->add(kPacket, std::move(e));
}

namespace {
template <typename Fn> EntryPtr entry(Hooks::Owner owner, Fn fn) {
    auto e = std::make_shared<Entry>();
    e->owner = owner;
    e->fn.template emplace<Fn>(std::move(fn));
    return e;
}
} // namespace

void Hooks::on_chat(Owner owner, std::function<Flow(ChatLine&)> fn) { impl_->add(kChat, entry(owner, std::move(fn))); }
void Hooks::on_command_line(Owner owner, std::function<Flow(std::string&)> fn) {
    impl_->add(kLine, entry(owner, std::move(fn)));
}
bool Hooks::add_command(Owner owner, std::string name, std::function<void(std::string_view)> fn,
                        std::uint32_t required) {
    if (name.empty()) return false;
    if (name.front() != '/') name.insert(name.begin(), '/');
    name = lower(name);
    const auto& g = impl_->guests;
    if (std::find(g.begin(), g.end(), owner) != g.end() && reserved_command(name)) {
        log::warn("plugin hooks: {} may not take {}: it is the client's own command", owner_name(owner), name);
        return false;
    }
    auto e = entry(owner, std::move(fn));
    e->name = std::move(name);
    e->required = required;
    impl_->add(kCommand, std::move(e));
    impl_->command_catalog_changed();
    return true;
}

bool Hooks::reserved_command(std::string_view name) const {
    std::string n = lower(name);
    if (!n.empty() && n.front() != '/') n.insert(n.begin(), '/');
    // A word with a space would never be looked up as one command; the game's table takes any prefix of its names.
    if (n.size() < 2 || n.find_first_of(" \t") != std::string::npos || game::find_slash_command(n, true)) return true;
    const auto& g = impl_->guests;
    for (const auto& e : impl_->lists[kCommand])
        if (e->alive && e->name == n && std::find(g.begin(), g.end(), e->owner) == g.end()) return true;
    return false;
}
void Hooks::on_pulse(Owner owner, std::function<void()> fn) { impl_->add(kPulse, entry(owner, std::move(fn))); }
void Hooks::on_game_state(Owner owner, std::function<void(int)> fn) { impl_->add(kState, entry(owner, std::move(fn))); }
void Hooks::on_zone(Owner owner, std::function<void(ZonePhase)> fn) { impl_->add(kZone, entry(owner, std::move(fn))); }
void Hooks::on_spawn(Owner owner, std::function<void(std::uint32_t, bool)> fn) {
    impl_->add(kSpawn, entry(owner, std::move(fn)));
}
void Hooks::on_imgui(Owner owner, std::function<void()> fn) { impl_->add(kImgui, entry(owner, std::move(fn))); }

bool Hooks::has_packet_hooks(Stream stream, Direction dir) const {
    for (const auto& e : impl_->lists[kPacket])
        if (e->alive && e->stream == stream && e->dir == dir) return true;
    return false;
}

Flow Hooks::packet(Stream stream, Direction dir, net::AppPacket& p) {
    if (impl_->packet_depth >= kMaxPacketDepth) {
        log::warn("plugin packet hooks: packets sent from packet hooks nest too deep, opcode {:#06x} dropped", p.opcode);
        return Flow::Drop;
    }
    ++impl_->packet_depth;
    bool dropped = impl_->each(kPacket, [&](Entry& e) {
        if (e.stream != stream || e.dir != dir || (e.opcode && e.opcode != p.opcode)) return false;
        return std::get<PacketFn>(e.fn)(p) == Flow::Drop;
    });
    --impl_->packet_depth;
    return dropped ? Flow::Drop : Flow::Continue;
}

Flow Hooks::chat(ChatLine& line) {
    bool dropped = impl_->each(kChat, [&](Entry& e) {
        return std::get<std::function<Flow(ChatLine&)>>(e.fn)(line) == Flow::Drop;
    });
    return dropped ? Flow::Drop : Flow::Continue;
}

bool Hooks::command(std::string& line, const std::function<bool(std::string_view)>& allow) {
    if (impl_->command_depth >= kMaxCommandDepth) {
        log::warn("plugin commands: commands run from commands nest too deep, '{}' dropped", line.substr(0, 64));
        return true;
    }
    struct Depth {
        int& d;
        explicit Depth(int& v) : d(v) { ++d; }
        ~Depth() { --d; }
        Depth(const Depth&) = delete;
        Depth& operator=(const Depth&) = delete;
    } depth(impl_->command_depth);
    if (impl_->each(kLine, [&](Entry& e) { return std::get<std::function<Flow(std::string&)>>(e.fn)(line) == Flow::Drop; }))
        return true;
    std::string_view rest = line;
    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    if (rest.empty() || rest.front() != '/') return false;
    std::size_t end = rest.find(' ');
    std::string word = lower(rest.substr(0, end));
    std::string_view args = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
    while (!args.empty() && args.front() == ' ') args.remove_prefix(1);
    // The latest live registration of the name.
    EntryPtr found;
    for (const auto& e : impl_->lists[kCommand])
        if (e->alive && e->name == word) found = e;
    if (!found) return false;
    if (allow && !allow(found->name)) return true;
    std::string held(args); // the line may change under the command
    ++impl_->depth;
    try {
        std::get<std::function<void(std::string_view)>>(found->fn)(held);
    } catch (const std::exception& ex) {
        log::warn("plugin command {} of {} failed: {}", word, impl_->owners[found->owner], ex.what());
    } catch (...) {
        log::warn("plugin command {} of {} failed", word, impl_->owners[found->owner]);
    }
    --impl_->depth;
    impl_->sweep();
    return true;
}

bool Hooks::has_command(std::string_view name) const {
    std::string n = lower(name);
    if (!n.empty() && n.front() != '/') n.insert(n.begin(), '/');
    for (const auto& e : impl_->lists[kCommand])
        if (e->alive && e->name == n) return true;
    return false;
}

std::optional<Hooks::Owner> Hooks::command_owner(std::string_view name) const {
    std::string n = lower(name);
    if (!n.empty() && n.front() != '/') n.insert(n.begin(), '/');
    std::optional<Owner> owner;
    for (const auto& e : impl_->lists[kCommand])
        if (e->alive && e->name == n) owner = e->owner;
    return owner;
}

bool Hooks::is_guest(Owner owner) const {
    const auto& g = impl_->guests;
    return std::find(g.begin(), g.end(), owner) != g.end();
}

std::uint32_t Hooks::command_required(std::string_view name) const {
    std::string n = lower(name);
    if (!n.empty() && n.front() != '/') n.insert(n.begin(), '/');
    std::uint32_t required = 0;
    for (const auto& e : impl_->lists[kCommand])
        if (e->alive && e->name == n) required = e->required;
    return required;
}

std::vector<std::string> Hooks::commands() const {
    std::vector<std::string> out;
    for (const auto& e : impl_->lists[kCommand])
        if (e->alive && std::find(out.begin(), out.end(), e->name) == out.end()) out.push_back(e->name);
    std::sort(out.begin(), out.end());
    return out;
}

std::uint32_t Hooks::command_catalog_serial() const { return impl_->command_catalog_serial; }

void Hooks::pulse() {
    impl_->each(kPulse, [](Entry& e) {
        std::get<std::function<void()>>(e.fn)();
        return false;
    });
}

void Hooks::game_state(int state) {
    impl_->each(kState, [&](Entry& e) {
        std::get<std::function<void(int)>>(e.fn)(state);
        return false;
    });
}

void Hooks::zone(ZonePhase phase) {
    impl_->each(kZone, [&](Entry& e) {
        std::get<std::function<void(ZonePhase)>>(e.fn)(phase);
        return false;
    });
}

void Hooks::spawn(std::uint32_t spawn_id, bool added) {
    impl_->each(kSpawn, [&](Entry& e) {
        std::get<std::function<void(std::uint32_t, bool)>>(e.fn)(spawn_id, added);
        return false;
    });
}

bool Hooks::has_spawn_hooks() const {
    for (const auto& e : impl_->lists[kSpawn])
        if (e->alive) return true;
    return false;
}

void Hooks::imgui() {
    impl_->each(kImgui, [](Entry& e) {
        std::get<std::function<void()>>(e.fn)();
        return false;
    });
}

} // namespace hailstone::plugin
