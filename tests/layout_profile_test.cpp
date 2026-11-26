#include <doctest/doctest.h>

#include <barrier>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/atomic_file.h"
#include "core/byte_io.h"
#include "scratch_dir.h"
#include "ui/layout_profile.h"

using namespace hailstone;

TEST_CASE("Layout profiles: durable save, name matching and malformed file handling") {
    test::ScratchDir scratch("layout_profiles");
    const auto directory = scratch.path() / "modern";
    ui::LayoutProfile profile;
    profile.name = "Raid Wide";
    profile.width = 1920;
    profile.height = 1080;
    profile.windows.set("GroupWindow", "Show", "1");
    profile.chat.set("TextColors", "Color1", "0");
    profile.pack_settings = "hud.pos.GroupWindow=1,2\n";
    profile.rects = {{"Group Window", 24, 8, 210, 90}, {"Chat", -20, 500, 400, 180}};

    CHECK(ui::save_layout_profile(directory, profile, false) == ui::LayoutProfileSave::Saved);
    const auto saved = ui::read_layout_profile(directory, "raid wide");
    REQUIRE(saved);
    CHECK(saved->name == profile.name);
    CHECK(saved->width == profile.width);
    CHECK(saved->height == profile.height);
    CHECK(saved->windows.to_string() == profile.windows.to_string());
    CHECK(saved->chat.to_string() == profile.chat.to_string());
    CHECK(saved->pack_settings == profile.pack_settings);
    CHECK(saved->rects == profile.rects);
    CHECK_FALSE(ui::read_layout_profile(scratch.path() / "gamepad", "Raid Wide"));
    CHECK_FALSE(ui::read_layout_profile(directory, "../modern"));

    profile.name = "RAID WIDE";
    profile.height = 800;
    CHECK(ui::save_layout_profile(directory, profile, false) == ui::LayoutProfileSave::Exists);
    REQUIRE(ui::read_layout_profile(directory, profile.name));
    CHECK(ui::read_layout_profile(directory, profile.name)->height == 1080);
    CHECK(ui::save_layout_profile(directory, profile, true) == ui::LayoutProfileSave::Saved);
    CHECK(ui::list_layout_profiles(directory) == std::vector<std::string>{profile.name});
    CHECK_FALSE(ui::valid_layout_profile_name(" invalid "));
    CHECK_FALSE(ui::valid_layout_profile_name("C:\\Desktop"));
    CHECK_FALSE(ui::valid_layout_profile_name(std::string(49, 'x')));

    const auto path = directory / "profile-raid wide.hslayout";
    const auto contents = read_text_file(path);
    REQUIRE(contents);
    CHECK(contents->starts_with("Hailstone layout profile 1\n"));
    REQUIRE(write_file_atomic(path, contents->substr(0, contents->size() - 1)));
    CHECK_FALSE(ui::read_layout_profile(directory, profile.name));
    CHECK(ui::list_layout_profiles(directory).empty());
}

TEST_CASE("Layout profiles: preview geometry ignores unusable bounds and rejects a corrupt record count") {
    test::ScratchDir scratch("layout_preview_validation");
    ui::LayoutProfile profile;
    profile.name = "Raid";
    profile.width = 1920;
    profile.height = 1080;
    profile.rects = {
        {"Hidden", 2000, 0, 60, 80}, {"Flat", 0, 2, 0, 20}, {"Overlarge", 0, 0, 2000000000, 20},
        {"Line\nBreak", 0, 0, 100, 80}, {std::string(129, 'a'), 0, 0, 100, 80}
    };
    REQUIRE(ui::save_layout_profile(scratch.path(), profile, false) == ui::LayoutProfileSave::Saved);
    REQUIRE(ui::read_layout_profile(scratch.path(), profile.name));
    CHECK(ui::read_layout_profile(scratch.path(), profile.name)->rects.empty());
    for (std::size_t i = 0; i < ui::kLayoutProfileMaxRects + 10; ++i)
        profile.rects.push_back({std::to_string(i), -20, 500, 400, 180});
    REQUIRE(ui::save_layout_profile(scratch.path(), profile, true) == ui::LayoutProfileSave::Saved);
    const auto loaded = ui::read_layout_profile(scratch.path(), profile.name);
    REQUIRE(loaded);
    REQUIRE(loaded->rects.size() == ui::kLayoutProfileMaxRects);
    CHECK(loaded->rects.front().name == "0");

    profile.rects.clear();
    REQUIRE(ui::save_layout_profile(scratch.path(), profile, true) == ui::LayoutProfileSave::Saved);
    const auto path = scratch.path() / "profile-raid.hslayout";
    auto bytes = read_text_file(path);
    REQUIRE(bytes);
    bytes->resize(bytes->size() - sizeof(std::uint32_t));
    ByteWriter count;
    count.le(static_cast<std::uint32_t>(ui::kLayoutProfileMaxRects + 1));
    bytes->append(reinterpret_cast<const char*>(count.data().data()), count.data().size());
    REQUIRE(write_file_atomic(path, *bytes));
    CHECK_FALSE(ui::read_layout_profile(scratch.path(), profile.name));
}

TEST_CASE("Layout profiles: current setup matches saved settings independently of name and preview geometry") {
    test::ScratchDir scratch("layout_match");
    ui::LayoutProfile saved;
    saved.name = "Raid";
    saved.width = 1920;
    saved.height = 1080;
    saved.windows.set("GroupWindow", "Show", "1");
    saved.windows.set("GroupWindow", "XPosWindowed", "125");
    saved.windows.set("ChatManager", "ChatWindow0_Name", "Main Chat   ");
    saved.chat.set("Options", "FilterGuildChat", "1");
    saved.pack_settings = "hud.pos.GroupWindow=26,19\n";
    saved.rects = {{"Group", 125, 50, 200, 100}};
    REQUIRE(ui::save_layout_profile(scratch.path(), saved, false) == ui::LayoutProfileSave::Saved);

    auto current = saved;
    current.name.clear();
    current.width = 1280;
    current.height = 800;
    current.rects = {{"Group", 125, 50, 300, 120}};
    current.windows.parse("[groupwindow]\nxposwindowed=125\nshow=1\n");
    current.windows.set("ChatManager", "ChatWindow0_Name", "Main Chat   ");
    CHECK(ui::has_saved_layout_profile(scratch.path(), current));
    current.windows.set("GroupWindow", "YPosWindowed", "20");
    CHECK_FALSE(ui::has_saved_layout_profile(scratch.path(), current));
    current.windows = saved.windows;
    current.chat.set("Options", "FilterGuildChat", "0");
    CHECK_FALSE(ui::has_saved_layout_profile(scratch.path(), current));
    current.chat = saved.chat;
    current.pack_settings = "hud.pos.GroupWindow=26,20\n";
    CHECK_FALSE(ui::has_saved_layout_profile(scratch.path(), current));
}

TEST_CASE("Layout profiles: two clients cannot silently replace a new name") {
    test::ScratchDir scratch("layout_profile_race");
    ui::LayoutProfile profile;
    profile.name = "Desktop";
    profile.width = 1280;
    profile.height = 720;
    std::barrier start(2);
    auto first = ui::LayoutProfileSave::Failed;
    auto second = ui::LayoutProfileSave::Failed;
    std::thread save([&] {
        start.arrive_and_wait();
        first = ui::save_layout_profile(scratch.path(), profile, false);
    });
    start.arrive_and_wait();
    second = ui::save_layout_profile(scratch.path(), profile, false);
    save.join();
    if (first != ui::LayoutProfileSave::Saved) std::swap(first, second);
    CHECK(first == ui::LayoutProfileSave::Saved);
    CHECK(second == ui::LayoutProfileSave::Exists);
}

TEST_CASE("Layout profiles: restoring placement and chat leaves unrelated character settings") {
    IniFile before;
    before.parse("[GroupWindow]\nXPosWindowed=42\nWidthWindowed=200\nOptions=3\n"
                 "[ChatManager]\nChatWindow0_Name=Raid\nNumWindows=0\n"
                 "[Main]\nContextMenus=0\n[PetInfoWindow]\nPetButtonState=1\n");
    const auto snapshot = ui::capture_layout_windows(before);
    CHECK(snapshot.get_int("GroupWindow", "XPosWindowed", 0) == 42);
    CHECK_FALSE(snapshot.get("GroupWindow", "Options"));
    CHECK_FALSE(snapshot.get("Main", "ContextMenus"));
    IniFile current = before;
    current.set_int("GroupWindow", "XPosWindowed", 777);
    current.set_int("RaidWindow", "XPosWindowed", 1);
    current.set_int("GroupWindow", "Options", 11);
    current.set("ChatManager", "ChatWindow1_Name", "Old tab");
    const auto restored = ui::replace_layout_windows(std::move(current), snapshot);
    CHECK(restored.get_int("GroupWindow", "XPosWindowed", 0) == 42);
    CHECK(restored.get_int("GroupWindow", "Options", 0) == 11);
    CHECK(restored.get_string("PetInfoWindow", "PetButtonState") == "1");
    CHECK_FALSE(restored.get("RaidWindow", "XPosWindowed"));
    CHECK_FALSE(restored.get("ChatManager", "ChatWindow1_Name"));
    CHECK(restored.get_string("ChatManager", "ChatWindow0_Name") == "Raid");
}
