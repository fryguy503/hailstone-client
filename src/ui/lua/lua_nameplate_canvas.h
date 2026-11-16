#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct lua_State;
namespace hailstone::game {
class NameplateCanvasRegistry;
}
namespace hailstone::ui::lua {

// A registration captures its caller, including the caller's source generation. The runner supplies its normal
// protected call, time budget and capability scope; the shared bridge never grants access to another service.
struct NameplateCanvasLuaOwner {
    std::string key;
    std::function<bool()> valid;
    std::function<bool()> ready;
    std::function<bool(lua_State *, int, int)> invoke;
};
struct NameplateCanvasLuaEnv {
    game::NameplateCanvasRegistry *registry = nullptr;
    // Optional host identity: replacing a host in a retained VM retires its old factories and callbacks.
    std::weak_ptr<void> owner_lifetime;
    std::function<NameplateCanvasLuaOwner(lua_State *)> owner;
    std::function<bool(std::uintptr_t)> texture_valid;
    std::function<void()> registered;
};

// Pushes {version=1,Register,Unregister,MeasureText,SpellIcon}. Register([name,]fn) -> opaque registration string;
// callbacks receive one complete current frame and return a plain array of local-coordinate models.
void push_nameplate_canvas(lua_State *L, NameplateCanvasLuaEnv env);
// Retires every registration before a VM closes (also performed by the state's private userdata finalizer).
void release_nameplate_canvas(lua_State *L);

} // namespace hailstone::ui::lua
