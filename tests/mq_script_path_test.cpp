#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "mq/lua_mq.h"
#include "scratch_dir.h"

namespace fs = std::filesystem;
using hailstone::mq::script_spelled_path;

TEST_CASE("MQ script paths: normalized requests resolve actual package case at every level") {
    hailstone::test::ScratchDir scratch("mq_script_path_case");
    const auto root = scratch.path();
    fs::create_directories(root / "MiXeD/LiB");
    const auto entry = root / "MiXeD/InIt.LuA", shared = root / "MiXeD/LiB/HeLpEr.LuA";
    std::ofstream(entry) << "-- entry";
    std::ofstream(shared) << "-- module";
    const auto found = script_spelled_path(root, "mixed/init.lua");
    REQUIRE(found);
    CHECK(fs::equivalent(*found, entry));
    const auto module = script_spelled_path(root, "MIXED/lib/helper.lua");
    REQUIRE(module);
    CHECK(fs::equivalent(*module, shared));
    if (!fs::exists(root / "mixed")) {
        CHECK(*found == entry);
        CHECK(*module == shared);
    }
    CHECK_FALSE(script_spelled_path(root, "mixed/missing.lua"));
    CHECK_FALSE(script_spelled_path(root, "unknown/init.lua"));
}

TEST_CASE("MQ script paths: exact components win and ambiguous case fallback is refused") {
    hailstone::test::ScratchDir scratch("mq_script_path_ambiguous");
    const auto root = scratch.path();
    fs::create_directories(root / "Alpha");
    const auto exact = root / "Alpha/init.lua";
    std::ofstream(exact) << "-- exact";
    const auto found = script_spelled_path(root, "Alpha/init.lua");
    REQUIRE(found);
    CHECK(*found == exact);
    if (fs::exists(root / "alpha")) {
        MESSAGE("case-insensitive filesystem: distinct case-only siblings are unavailable");
        return;
    }
    fs::create_directories(root / "ALPHA");
    std::ofstream(root / "ALPHA/init.lua") << "-- other";
    CHECK_FALSE(script_spelled_path(root, "alpha/init.lua"));
    CHECK(script_spelled_path(root, "Alpha/init.lua") == std::optional<fs::path>{exact});
    std::ofstream(root / "Alpha/Util.lua") << "-- exact module";
    std::ofstream(root / "Alpha/UTIL.LUA") << "-- ambiguous module";
    CHECK_FALSE(script_spelled_path(root, "Alpha/util.lua"));
    CHECK(script_spelled_path(root, "Alpha/Util.lua") == std::optional<fs::path>{root / "Alpha/Util.lua"});
}

TEST_CASE("MQ script paths: public resolver rejects rooted and traversal paths before searching") {
    hailstone::test::ScratchDir scratch("mq_script_path_bounds");
    const auto root = scratch.path();
    std::ofstream(root / "entry.lua") << "-- entry";
    for (const auto& relative : {fs::path{}, fs::path("."), fs::path("./entry.lua"), fs::path("../entry.lua"),
                                 fs::path("folder/../entry.lua"), fs::path("/entry.lua"), fs::path("\\entry.lua"),
                                 fs::path("C:entry.lua"), fs::path("entry.lua:stream"),
                                 fs::path(std::string("entry.lua\0tail", 14))}) {
        CAPTURE(relative);
        CHECK_FALSE(script_spelled_path(root, relative));
    }
    CHECK_FALSE(script_spelled_path(root, root / "entry.lua"));
}
