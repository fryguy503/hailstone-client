// A C++ test plugin built with the SDK's C++ wrapper (sdk/hailstone_plugin.hpp), for tests/native_plugins_test.cpp:
// commands and TLOs as capturing lambdas, the readers, and the wrapper's lifetimes.
//   /cppgreet, /cpphello [args]  one handler on an object, capturing `this` and its own name (hs::Commands)
//   /cppreplace      replaces itself while it runs, then says so
//   /cppremoveself   removes itself while it runs
//   /cppthrow        throws (the wrapper catches it)
//   /cppdrop         destroys the object holding /cppgreet and /cpphello
//   /cppretlo text   replaces ${CppWrap}'s answer with `text`
//   /cppplayer       hs::GetPlayer and hs::GetSpawn of the player
//   /cppexists name  hs::CommandExists
//   ${CppWrap}, ${CppWrap[index].Index}, ${CppWrap.Calls}
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "hailstone_plugin.h"
#include "hailstone_plugin.hpp"

HAILSTONE_PLUGIN

namespace {

class Greeter {
public:
    explicit Greeter(std::string word) : word_(std::move(word)) {
        for (std::string name : {"/cppgreet", "/cpphello"})
            commands_.add(name, [this, name](const char* args) {
                ++calls_;
                WriteChatf("[Cpp] %s %s '%s' %d", word_.c_str(), name.c_str(), args, calls_);
            });
    }

private:
    std::string word_;
    int calls_ = 0;
    hs::Commands commands_; // last, so its commands go before what they capture
};

std::unique_ptr<Greeter> greeter;
int tlo_calls = 0;

bool answer(std::string_view text, const char* index, const char* member, HailstoneValue& out) {
    ++tlo_calls;
    if (!member) {
        out = hs::String(text.data());
        return true;
    }
    const std::string_view m = member;
    if (m == "Index") {
        out = hs::String(index);
        return true;
    }
    if (m == "Calls") {
        out = hs::Int(tlo_calls);
        return true;
    }
    return false;
}

} // namespace

HAILSTONE_API void InitializePlugin(void) {
    greeter = std::make_unique<Greeter>("hi");
    hs::AddCommand("/cppreplace", [](const char*) {
        hs::AddCommand("/cppreplace", [](const char*) { WriteChatf("[Cpp] replaced"); });
        WriteChatf("[Cpp] original"); // still here: the call keeps its callable
    });
    hs::AddCommand("/cppremoveself", [](const char*) {
        const bool removed = hs::RemoveCommand("/CPPREMOVESELF");
        WriteChatf("[Cpp] removed %d", removed ? 1 : 0);
    });
    hs::AddCommand("/cppthrow", [](const char*) { throw std::runtime_error("thrown from a command"); });
    hs::AddCommand("/cppdrop", [](const char*) {
        greeter.reset();
        WriteChatf("[Cpp] dropped");
    });
    hs::AddCommand("/cppretlo", [](const char* args) {
        const bool added = hs::AddTopLevelObject("cppwrap", [text = std::string(args)](const char* index, const char* member,
                                                                                      const char*, HailstoneValue& out) {
            return answer(text, index, member, out);
        });
        WriteChatf("[Cpp] retlo %d", added ? 1 : 0);
    });
    hs::AddCommand("/cppplayer", [](const char*) {
        const auto player = hs::GetPlayer();
        const auto spawn = player ? hs::GetSpawn(player->spawn_id) : std::nullopt;
        WriteChatf("[Cpp] player %s %u spawn %s", player ? player->name : "none",
                   static_cast<unsigned>(player ? player->spawn_id : 0), spawn ? "yes" : "no");
    });
    hs::AddCommand("/cppexists", [](const char* args) {
        const auto kind = hs::CommandExists(args);
        WriteChatf("[Cpp] exists %d", kind ? *kind : -1);
    });
    hs::AddTopLevelObject("CppWrap", [](const char* index, const char* member, const char*, HailstoneValue& out) {
        return answer("first", index, member, out);
    });
}

HAILSTONE_API void ShutdownPlugin(void) {
    greeter.reset();
    hs::RemoveAllCommands();
    hs::RemoveAllTopLevelObjects();
}
