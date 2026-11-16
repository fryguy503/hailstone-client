#include <doctest/doctest.h>

#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <lua.hpp>

#include "game/nameplate_canvas.h"
#include "ui/lua/lua_nameplate_canvas.h"

namespace {
using namespace hailstone::game;
using namespace hailstone::ui::lua;

NameplateCanvasFrame canvas_frame(std::size_t count = 1) {
    NameplateCanvasFrame f;
    f.viewport = {0, 0, 640, 480};
    f.now_ms = 100;
    f.dt_ms = 16;
    for (std::size_t i = 0; i < count; ++i) {
        NameplateCanvasCandidate c;
        c.spawn.id = static_cast<std::uint32_t>(i + 1);
        c.spawn.incarnation = 17;
        c.spawn.name = "Example_unit01";
        c.spawn.npc = true;
        c.context.name = "Example unit";
        c.context.hp_percent = 60;
        c.x = 200;
        c.y = 100;
        c.depth = 40;
        f.candidates.push_back(std::move(c));
    }
    return f;
}
constexpr const char *kModel = R"lua(
function model(f)
 return {token=f.candidates[1].token,bounds={x=-20,y=-10,w=40,h=10},commands={{kind='RectFilled',x0=-20,y0=-10,x1=20,y1=0,color=0x123456}}}
end
)lua";
struct CanvasLua {
    std::unique_ptr<NameplateCanvasRegistry> registry = std::make_unique<NameplateCanvasRegistry>();
    lua_State *L = luaL_newstate();
    bool allowed = true, ready = true, registered = false;
    bool throw_valid = false;
    std::function<void()> on_capture;
    int calls = 0;
    std::string owner = "example";
    std::shared_ptr<void> generation = std::make_shared<int>(0);
    CanvasLua() {
        luaL_openlibs(L);
        registry->set_measure([](std::string_view s, float size, int) {
            return CanvasTextMetrics{static_cast<float>(s.size()) * size,     size,    -1, -2,
                                     static_cast<float>(s.size()) * size + 1, size + 2};
        });
        registry->set_icon([](std::uint32_t id) -> std::optional<CanvasImage> {
            if (id != 37)
                return std::nullopt;
            return CanvasImage{123, 0.1f, 0.2f, 0.3f, 0.4f};
        });
        NameplateCanvasLuaEnv env;
        env.registry = registry.get();
        env.registered = [this] { registered = true; };
        env.texture_valid = [](std::uintptr_t t) { return t == 123; };
        env.owner = [this](lua_State *) {
            if (auto callback = std::exchange(on_capture, {})) callback();
            const std::weak_ptr<void> source = generation;
            NameplateCanvasLuaOwner out;
            out.key = owner;
            out.valid = [this, source] {
                if (throw_valid) throw std::runtime_error("owner query failed");
                return allowed && !source.expired();
            };
            out.ready = [this] { return ready; };
            out.invoke = [this](lua_State *s, int args, int results) {
                ++calls;
                const int result = lua_pcall(s, args, results, 0);
                if (result != LUA_OK)
                    lua_pop(s, 1);
                return result == LUA_OK;
            };
            return out;
        };
        push_nameplate_canvas(L, std::move(env));
        lua_setglobal(L, "np");
        code(kModel);
    }
    ~CanvasLua() {
        if (L) {
            release_nameplate_canvas(L);
            lua_close(L);
        }
    }
    bool code(const std::string &code) {
        const int top = lua_gettop(L);
        const int status = luaL_dostring(L, code.c_str());
        if (status != LUA_OK)
            INFO(lua_tostring(L, -1));
        lua_settop(L, top);
        return status == LUA_OK;
    }
    auto frame(std::size_t count = 1) {
        auto f = canvas_frame(count);
        return registry->frame(f);
    }
};
} // namespace

TEST_CASE("nameplate canvas API: frame tokens and known context reach a generic renderer") {
    CanvasLua h;
    REQUIRE(h.code(R"lua(
assert(np.version==1)
handle=np.Register('example',function(f)
 assert(f.serial>0 and f.viewport.w==640 and f.font_size==16)
 assert(f.candidates[1].spawn.class==0 and f.candidates[1].spawn.incarnation==17)
 assert(f.candidates[1].context.hp_percent==60 and f.candidates[1].context.mana_percent==nil)
 assert(f.candidates[1].context.now_ms==100 and f.candidates[1].context.dt_ms==16)
 return {model(f)}
end)
assert(type(handle)=='string')
)lua"));
    auto result = h.frame();
    REQUIRE(result.size() == 1);
    CHECK(h.registered);
    CHECK(h.calls == 1);
    CHECK(result[0].candidate_index == 0);
    CHECK(result[0].model.commands[0].color == 0x123456);
    CHECK(result[0].model.clickable);
    CHECK_FALSE(result[0].model.through_walls);
    CHECK(h.registry->owner_valid(result[0].owner));
    REQUIRE(h.code("assert(np.Unregister(handle));assert(not np.Unregister(handle))"));
    CHECK(h.frame().empty());
    CHECK_FALSE(h.registry->owner_valid(result[0].owner));
}

TEST_CASE("nameplate canvas API: stale duplicate and foreign tokens never claim current entities") {
    CanvasLua h;
    REQUIRE(
        h.code("handle=np.Register(function(f) local m=model(f);saved=saved or m.token;m.token=saved;return {m} end)"));
    REQUIRE(h.frame().size() == 1);
    CHECK(h.frame().empty());
    REQUIRE(h.code("np.Register(function(f) local m=model(f);return {m,m} end)"));
    CHECK(h.frame().empty());
    REQUIRE(h.code("np.Register(function(f) local m=model(f);m.token='foreign';return {m} end)"));
    CHECK(h.frame().empty());
}

TEST_CASE("nameplate canvas API: malformed raw data fails closed without invoking metamethods") {
    const std::string mutations[] = {
        "return setmetatable({m},{__len=function() error('called') end})",
        "return {setmetatable(m,{__index=function() error('called') end})}",
        "m.bounds=setmetatable(m.bounds,{__index=function() error('called') end});return {m}",
        "m.commands=setmetatable(m.commands,{__len=function() error('called') end});return {m}",
        "m.commands[1]=setmetatable(m.commands[1],{});return {m}",
        "m.bounds.w=4097;return {m}",
        "m.bounds.x=1e300;return {m}",
        "m.bounds.w=0/0;return {m}",
        "m.offset_y=math.huge;return {m}",
        "m.clickable=1;return {m}",
        "m.through_walls='true';return {m}",
        "m.commands[1].kind='Bar';return {m}",
        "m.commands[1].color=1.5;return {m}",
        "m.commands[1].color=-1;return {m}",
        "m.commands[1].alpha=2;return {m}",
        "m.commands[1].x0='4';return {m}",
        "m.commands[1].x0=1e300;return {m}",
        "m.commands[1].kind='Text';m.commands[1].text=string.rep('x',1025);return {m}",
        "m.commands[1].kind='Text';m.commands[1].text='a\\0b';return {m}",
        "m.commands[1].kind='Image';m.commands[1].texture=123;return {m}",
        "m.commands[3]=m.commands[1];return {m}",
        "return {[2]=m}",
        "return {m,bad=true}",
        "m.commands={};return {m}",
        "return false",
    };
    CanvasLua h;
    for (const auto &mutation : mutations) {
        CAPTURE(mutation);
        REQUIRE(h.code("np.Register('test',function(f)local m=model(f);" + mutation + " end)"));
        CHECK(h.frame().empty());
        CHECK(lua_gettop(h.L) == 0);
    }
}

TEST_CASE("nameplate canvas API: primitive vocabulary supports live images and explicit hide") {
    CanvasLua h;
    REQUIRE(h.code(R"lua(
np.Register(function(f)
 local m=model(f)
 for _,kind in ipairs({'Rect','RectFilled','Gradient','Line','Circle','CircleFilled','Triangle','TriangleFilled','Text','Image'}) do
  local texture=np.SpellIcon(37)
  m.commands[#m.commands+1]={kind=kind,x=0,y=0,x0=0,y0=0,x1=1,y1=1,x2=1,y2=0,radius=1,text='Known',texture=texture}
 end
 return {m}
end)
)lua"));
    auto result = h.frame();
    REQUIRE(result.size() == 1);
    CHECK(result[0].model.commands.size() == 11);
    REQUIRE(h.code("np.Register(function(f)return {{token=f.candidates[1].token,hide=true}} end)"));
    result = h.frame();
    REQUIRE(result.size() == 1);
    CHECK(result[0].model.hide);
    CHECK(result[0].model.commands.empty());
}

TEST_CASE("nameplate canvas API: metrics and images retain bounds unknown values and permissions") {
    CanvasLua h;
    REQUIRE(h.code(R"lua(
local m=np.MeasureText('abc',10,0)
assert(m.width==30 and m.height==10 and m.ink_x0==-1 and m.ink_y1==12)
assert(np.MeasureText('abc',1e300)==nil and np.MeasureText('abc',0/0)==nil)
assert(np.MeasureText(string.rep('x',1025),10)==nil)
local t,u0,v0,u1,v1=np.SpellIcon(37)
assert(type(t)=='userdata' and math.abs(u0-.1)<.0001 and math.abs(v1-.4)<.0001)
assert(np.SpellIcon(38)==nil and np.SpellIcon(65536)==nil)
)lua"));
    h.allowed = false;
    CHECK_FALSE(h.code("np.Register(function()end)"));
    CHECK_FALSE(h.code("np.MeasureText('text',10)"));
    CHECK_FALSE(h.code("np.SpellIcon(37)"));
    CHECK(h.registry->empty());
}

TEST_CASE("nameplate canvas API: owner guards prevent callbacks after disable or source retirement") {
    CanvasLua h;
    REQUIRE(h.code("savedRegister=np.Register;handle=np.Register(function(f)return {model(f)} end)"));
    REQUIRE(h.frame().size() == 1);
    const int before = h.calls;
    h.allowed = false;
    CHECK(h.frame().empty());
    CHECK(h.calls == before);
    CHECK_FALSE(h.code("savedRegister(function()end)"));
    h.allowed = true;
    h.generation.reset();
    CHECK(h.frame().empty());
    CHECK(h.calls == before);
    CHECK(h.registry->empty());
    CHECK_FALSE(h.code("savedRegister(function()end)"));
    release_nameplate_canvas(h.L);
    CHECK(h.frame().empty());
    CHECK(h.code("local handle,why=savedRegister(function()end);assert(handle==nil and type(why)=='string')"));
    CHECK(h.registry->empty());
    CHECK(h.calls == before);
}

TEST_CASE("nameplate canvas API: self-unregister and replacement cannot commit the retired callback") {
    CanvasLua h;
    REQUIRE(h.code("handle=np.Register('self',function(f)np.Unregister(handle);return {model(f)} end)"));
    CHECK(h.frame().empty());
    CHECK(h.registry->empty());
    REQUIRE(h.code("np.Register('replace',function(f)np.Register('replace',function(g)return {model(g)} end);return "
                   "{model(f)} end)"));
    CHECK(h.frame().empty());
    CHECK(h.frame().size() == 1);
}

TEST_CASE("nameplate canvas API: a callback failure detaches and both destruction orders are safe") {
    CanvasLua h;
    REQUIRE(h.code("np.Register(function()error('renderer failed')end)"));
    CHECK(h.frame().empty());
    CHECK(h.registry->empty());
    const int calls = h.calls;
    CHECK(h.frame().empty());
    CHECK(h.calls == calls);
    REQUIRE(h.code("np.Register(function(f)return {model(f)}end)"));
    h.registry.reset();
    CHECK(h.code("assert(np.Register(function()end)==nil)"));
    release_nameplate_canvas(h.L);
    NameplateCanvasRegistry registry;
    {
        CanvasLua other;
        REQUIRE(other.code("np.Register(function(f)return {model(f)}end)"));
        lua_close(other.L);
        other.L = nullptr;
        CHECK(other.registry->empty());
    }
    CHECK(registry.empty());
}

TEST_CASE("nameplate canvas API: unregister rechecks identity after a reentrant owner capture") {
    CanvasLua h;
    REQUIRE(h.code("handle=np.Register(function(f)return {model(f)}end)"));
    bool removed = false;
    h.on_capture = [&] { removed = h.code("assert(np.Unregister(handle))"); };
    CHECK(h.code("assert(not np.Unregister(handle))"));
    CHECK(removed);
    CHECK(h.registry->empty());
    CHECK(h.frame().empty());
    CHECK(lua_gettop(h.L) == 0);
}

TEST_CASE("nameplate canvas API: owner query exceptions become protected Lua errors") {
    CanvasLua h;
    REQUIRE(h.code("handle=np.Register(function(f)return {model(f)}end)"));
    h.throw_valid = true;
    CHECK(h.code(R"lua(
for _,call in ipairs({
 function()np.Register('late',function()end)end,
 function()np.Unregister(handle)end,
 function()np.MeasureText('value')end,
 function()np.SpellIcon(37)end,
}) do
 local ok,why=pcall(call)
 assert(not ok and why:find('owner query failed',1,true))
end
)lua"));
    h.throw_valid = false;
    CHECK(h.frame().size() == 1);
    CHECK(h.code("assert(np.Unregister(handle))"));
    CHECK(h.registry->empty());
    CHECK(lua_gettop(h.L) == 0);
}

TEST_CASE("nameplate canvas API: frame construction memory failures stay inside the protected call") {
    CanvasLua h;
    REQUIRE(h.code("np.Register(function(f)return {model(f)}end)"));
    struct Allocator {
        lua_Alloc original;
        void *user;
    } allocator{};
    allocator.original = lua_getallocf(h.L, &allocator.user);
    const auto refuse_growth = [](void *context, void *pointer, std::size_t old_size, std::size_t new_size) -> void * {
        auto &a = *static_cast<Allocator *>(context);
        if (new_size > old_size)
            return nullptr;
        return a.original(a.user, pointer, old_size, new_size);
    };
    lua_setallocf(h.L, refuse_growth, &allocator);
    const auto result = h.frame(1000);
    lua_setallocf(h.L, allocator.original, allocator.user);
    CHECK(result.empty());
    CHECK(h.registry->empty());
    CHECK(lua_gettop(h.L) == 0);
}

TEST_CASE("nameplate canvas API: repeated named replacement releases callback references") {
    CanvasLua h;
    REQUIRE(h.code(R"lua(
refs=setmetatable({},{__mode='v'})
for i=1,1000 do
 local f=function(frame)local m=model(frame);m.offset_x=i%10;return {m}end
 refs[i]=f;assert(np.Register('same',f))
end
collectgarbage();collectgarbage()
local count=0;for _ in pairs(refs) do count=count+1 end
assert(count==1)
for i=1,31 do assert(np.Register('slot'..i,function(f)return nil end)) end
assert(np.Register('same',function(f)return {model(f)} end))
assert(np.Register('overflow',function()end)==nil)
)lua"));
    CHECK(h.frame().size() == 1);
}

TEST_CASE("nameplate canvas API: candidate model and command budgets fail closed") {
    CanvasLua h;
    REQUIRE(h.code(R"lua(
np.Register(function(f)
 local out={}
 for _,c in ipairs(f.candidates) do
  local m=model(f);m.token=c.token
  for i=2,512 do m.commands[i]=m.commands[1] end
  out[#out+1]=m
 end
 return out
end)
)lua"));
    CHECK(h.frame(64).size() == 64);
    CHECK(h.frame(65).size() == 64);
    const int before = h.calls;
    CHECK(h.frame(kCanvasMaxCandidates + 1).empty());
    CHECK(h.calls == before);
    REQUIRE(h.code(
        "np.Register(function(f)local m=model(f);for i=2,513 do m.commands[i]=m.commands[1] end;return {m}end)"));
    CHECK(h.frame().empty());
}

TEST_CASE("nameplate canvas API: weighted geometry budgets bound text and reserve target capacity") {
    CanvasLua h;
    REQUIRE(h.code(R"lua(
np.Register(function(f)
 local out={}
 for _,c in ipairs(f.candidates) do
  local m=model(f);m.token=c.token;m.commands={}
  for i=1,15 do m.commands[i]={kind='Text',text=string.rep('x',1024),size=16}end
  out[#out+1]=m
 end
 return out
end)
)lua"));
    auto f = canvas_frame(10);
    f.candidates.back().context.target = true;
    auto out = h.registry->frame(f);
    REQUIRE(out.size() == 4);
    CHECK(out.front().candidate_index == 9);
    std::size_t vertices = 0;
    for (const auto &s : out) {
        const auto cost = canvas_model_vertices(s.model);
        CHECK(cost <= kCanvasMaxModelVertices);
        vertices += cost;
    }
    CHECK(vertices <= kCanvasMaxFrameVertices);
    REQUIRE(h.code("np.Register(function(f)local m=model(f);m.commands={};for i=1,16 do "
                   "m.commands[i]={kind='Text',text=string.rep('x',1024)}end;return {m}end)"));
    CHECK(h.frame().empty());
}

TEST_CASE("nameplate canvas registry: native owners share current-frame validation and reentrant guards") {
    NameplateCanvasRegistry registry;
    NameplateCanvasOwner owner = 0;
    owner = registry.add(
        "self", [](const auto &) -> std::optional<std::vector<NameplateCanvasModel>> { return std::nullopt; },
        [&] {
            registry.remove(owner);
            return true;
        });
    CHECK_FALSE(registry.owner_valid(owner));
    CHECK_FALSE(registry.owner_valid(owner));
    for (std::size_t i = 0; i < kCanvasMaxOwners; ++i)
        REQUIRE(registry.add(std::to_string(i), [](const auto &) -> std::optional<std::vector<NameplateCanvasModel>> {
            return std::nullopt;
        }));
    CHECK(registry.add(
              "0", [](const auto &) -> std::optional<std::vector<NameplateCanvasModel>> { return std::nullopt; }) != 0);
    CHECK(registry.add("overflow", [](const auto &) -> std::optional<std::vector<NameplateCanvasModel>> {
        return std::nullopt;
    }) == 0);
    CanvasCommand command;
    command.x = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(valid_canvas_command(command));
    command = {};
    command.kind = static_cast<CanvasCommandKind>(500);
    CHECK_FALSE(valid_canvas_command(command));
}
