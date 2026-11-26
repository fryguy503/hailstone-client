/* SPDX-License-Identifier: MIT (sdk/LICENSE) */
#pragma once

// C++ conveniences over hailstone_plugin.h (C++20, header only, optional). Commands and TLOs take any callable, a
// lambda with captures among them, as MacroQuest's mq::AddCommand does:
//
//     hs::AddCommand(name, [this, name](const char* args) { ... });
//
// and the readers fill the struct's size themselves. Only C function pointers and void* cross into the client, so
// a plugin built with this header loads in any client of its ABI, whatever compiler built either. Include it after
// hailstone_plugin.h's HAILSTONE_PLUGIN source, or in any source of the plugin; C plugins ignore it.
//
// The callables live in this plugin, one registry per plugin binary. Call everything on the client's thread, from
// inside a callback (InitializePlugin, OnPulse, a command ...), as the host table asks. Remove what captures an object
// before the object goes: hs::Commands does that for you; otherwise RemoveAllCommands / RemoveAllTopLevelObjects in
// ShutdownPlugin. The client also drops everything a plugin registered when it unloads it.

#ifndef __cplusplus
#error "hailstone_plugin.hpp is C++; C plugins use hailstone_plugin.h"
#endif

#include "hailstone_plugin.h"

#ifdef HAILSTONE_HOST
#error "hailstone_plugin.hpp is for plugins; the client defines HAILSTONE_HOST"
#endif

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hs {

// A command's body: the rest of the typed line after the command's name ("" with none).
using CommandFn = std::function<void(const char* args)>;
// A TLO's answer: ${Name[index]} with member and member_index null, ${Name[index].Member[member_index]} with them
// (index and member_index "" when not given). True with `out` filled, false for nil.
using TloFn = std::function<bool(const char* index, const char* member, const char* member_index, HailstoneValue& out)>;

namespace detail {

inline char ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// The client's own keys: commands lower case with their '/', TLOs without case.
inline std::string command_key(std::string_view name) {
    std::string key;
    key.reserve(name.size() + 1);
    if (name.empty() || name.front() != '/') key.push_back('/');
    for (char c : name) key.push_back(ascii_lower(c));
    return key;
}
inline std::string tlo_key(std::string_view name) {
    std::string key;
    key.reserve(name.size());
    for (char c : name) key.push_back(ascii_lower(c));
    return key;
}

// What the client's `user` points at. The callable is shared so that a call in progress keeps it alive when the
// handler removes or replaces itself.
template <typename Fn> struct Slot {
    std::string name; // as given, for the host calls
    std::shared_ptr<const Fn> fn;
    std::uint64_t serial = 0; // which registration this is (a freed slot's address may come back)
};

struct Registry {
    std::map<std::string, std::unique_ptr<Slot<CommandFn>>> commands;
    std::map<std::string, std::unique_ptr<Slot<TloFn>>> tlos;
    std::uint64_t next_serial = 1;
};
// Freed when the plugin unloads, after the client has dropped every registration: it makes no host calls.
inline Registry& registry() {
    static Registry r;
    return r;
}

inline void command_trampoline(void* user, const char* args) noexcept {
    try {
        std::shared_ptr<const CommandFn> fn = static_cast<Slot<CommandFn>*>(user)->fn; // the slot may go during the call
        if (fn && *fn) (*fn)(args ? args : "");
    } catch (...) {
        // An exception must not cross into the client (hailstone_plugin.h, "Exceptions").
    }
}

inline int tlo_trampoline(void* user, const char* index, const char* member, const char* member_index,
                          HailstoneValue* out) noexcept {
    try {
        std::shared_ptr<const TloFn> fn = static_cast<Slot<TloFn>*>(user)->fn;
        return out && fn && *fn && (*fn)(index ? index : "", member, member_index, *out) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

} // namespace detail

// ---- commands

// MQ's AddCommand with a callable. False without the 'commands' capability, for one of the client's own commands
// (the game's, /echo ...), or with no callable. Adding a name this plugin already has replaces it; when that fails,
// the earlier one is gone too.
inline bool AddCommand(std::string_view name, CommandFn fn) {
    if (!fn) return false;
    auto& commands = detail::registry().commands;
    std::string key = detail::command_key(name);
    if (auto it = commands.find(key); it != commands.end()) {
        // The client keeps every registration of a name until it is removed; one, not a stack, is this one's.
        ::RemoveCommand(it->second->name.c_str());
        commands.erase(it);
    }
    auto slot = std::make_unique<detail::Slot<CommandFn>>();
    slot->name = std::string(name);
    slot->fn = std::make_shared<const CommandFn>(std::move(fn));
    slot->serial = detail::registry().next_serial++;
    if (!::AddCommand(slot->name.c_str(), &detail::command_trampoline, slot.get())) return false;
    commands.emplace(std::move(key), std::move(slot));
    return true;
}

// False when this plugin had no such command (added through this header).
inline bool RemoveCommand(std::string_view name) {
    auto& commands = detail::registry().commands;
    auto it = commands.find(detail::command_key(name));
    if (it == commands.end()) return false;
    ::RemoveCommand(it->second->name.c_str());
    commands.erase(it);
    return true;
}

inline void RemoveAllCommands() {
    auto& commands = detail::registry().commands;
    for (auto& [key, slot] : commands) ::RemoveCommand(slot->name.c_str());
    commands.clear();
}

// Whether a typed "/name" would run a command (host->command_exists): HAILSTONE_COMMAND_ADDED,
// HAILSTONE_COMMAND_GAME or HAILSTONE_COMMAND_NONE. Empty on a client without command_exists.
inline std::optional<int> CommandExists(std::string_view name) {
    if (!HAILSTONE_HOST_HAS(host, command_exists)) return std::nullopt;
    return host->command_exists(host->host, std::string(name).c_str());
}

// host->lend_thread for a scope: while it lives, the plugin's own threads may call the host (one call at a time) as
// if on the client's thread, for a callback that waits on them. Make it inside a callback, on the client's thread.
// ok() is false where the client refuses (see lend_thread) or lacks it. Nested in one that lends already, it lends too
// and leaves the ending to the outer one.
class LendThread {
public:
    LendThread() : state_(HAILSTONE_HOST_HAS(host, lend_thread) ? host->lend_thread(host->host, 1) : 0) {}
    ~LendThread() {
        if (state_ == 1) host->lend_thread(host->host, 0);
    }
    LendThread(const LendThread&) = delete;
    LendThread& operator=(const LendThread&) = delete;
    bool ok() const { return state_ != 0; }

private:
    int state_; // lend_thread(1)'s answer: 1 started here, 2 already lent, 0 refused
};

// ---- TLOs

// MQ's AddTopLevelObject with a callable. False when the name is the client's or another plugin's TLO, or with no
// callable. Adding a name this plugin already has replaces its answer in place.
inline bool AddTopLevelObject(std::string_view name, TloFn fn) {
    if (!fn) return false;
    auto& tlos = detail::registry().tlos;
    std::string key = detail::tlo_key(name);
    if (auto it = tlos.find(key); it != tlos.end()) {
        it->second->fn = std::make_shared<const TloFn>(std::move(fn)); // still this plugin's: nobody else may take it
        return true;
    }
    auto slot = std::make_unique<detail::Slot<TloFn>>();
    slot->name = std::string(name);
    slot->fn = std::make_shared<const TloFn>(std::move(fn));
    if (!::AddTopLevelObject(slot->name.c_str(), &detail::tlo_trampoline, slot.get())) return false;
    tlos.emplace(std::move(key), std::move(slot));
    return true;
}

inline bool RemoveTopLevelObject(std::string_view name) {
    auto& tlos = detail::registry().tlos;
    auto it = tlos.find(detail::tlo_key(name));
    if (it == tlos.end()) return false;
    ::RemoveTopLevelObject(it->second->name.c_str());
    tlos.erase(it);
    return true;
}

inline void RemoveAllTopLevelObjects() {
    auto& tlos = detail::registry().tlos;
    for (auto& [key, slot] : tlos) ::RemoveTopLevelObject(slot->name.c_str());
    tlos.clear();
}

// HailstoneValue makers for a TLO's answer. A string is copied by the client when the TLO returns, so it must live
// until then (a member of the plugin, or a static), not a temporary.
inline HailstoneValue Bool(bool v) { return {HAILSTONE_BOOL, v ? 1 : 0, 0.0, nullptr}; }
inline HailstoneValue Int(std::int64_t v) { return {HAILSTONE_INT, v, 0.0, nullptr}; }
inline HailstoneValue Float(double v) { return {HAILSTONE_FLOAT, 0, v, nullptr}; }
inline HailstoneValue String(const char* v) { return {HAILSTONE_STRING, 0, 0.0, v}; }

// ---- owning registrations

// Commands that go with their owner: hold one as a member of the object the lambdas capture, and its commands are
// removed when the object is. A name added through it and replaced since (through another Commands or
// hs::AddCommand) is the replacement's, and stays.
class Commands {
public:
    Commands() = default;
    Commands(const Commands&) = delete;
    Commands& operator=(const Commands&) = delete;
    ~Commands() { clear(); }

    bool add(std::string_view name, CommandFn fn) {
        if (!AddCommand(name, std::move(fn))) return false;
        std::string key = detail::command_key(name);
        const std::uint64_t serial = detail::registry().commands.at(key)->serial;
        for (auto& [k, s] : added_)
            if (k == key) {
                s = serial;
                return true;
            }
        added_.emplace_back(std::move(key), serial);
        return true;
    }

    void clear() {
        auto& commands = detail::registry().commands;
        for (const auto& [key, serial] : added_) {
            auto it = commands.find(key);
            if (it != commands.end() && it->second->serial == serial) {
                ::RemoveCommand(it->second->name.c_str());
                commands.erase(it);
            }
        }
        added_.clear();
    }

private:
    std::vector<std::pair<std::string, std::uint64_t>> added_; // key and serial
};

// ---- readers (HAILSTONE_CAP_READ): the struct's size filled in, empty when the client gives nothing

inline std::optional<HailstonePlayer> GetPlayer() {
    HailstonePlayer p{};
    p.size = sizeof p;
    if (!HAILSTONE_HOST_HAS(host, get_player) || !host->get_player(host->host, &p)) return std::nullopt;
    return p;
}

inline std::optional<HailstoneSpawn> GetSpawn(std::uint32_t spawn_id) {
    HailstoneSpawn s{};
    s.size = sizeof s;
    if (!HAILSTONE_HOST_HAS(host, get_spawn) || !host->get_spawn(host->host, spawn_id, &s)) return std::nullopt;
    return s;
}

// The spawn with the lowest id above after_id (0 starts): `for (auto s = hs::NextSpawn(0); s; s = hs::NextSpawn(s->id))`.
inline std::optional<HailstoneSpawn> NextSpawn(std::uint32_t after_id) {
    HailstoneSpawn s{};
    s.size = sizeof s;
    if (!HAILSTONE_HOST_HAS(host, next_spawn) || !host->next_spawn(host->host, after_id, &s)) return std::nullopt;
    return s;
}

} // namespace hs
