#include "app/ui_demo.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <stdexcept>
#include <system_error>

#include <imgui.h>

#include "app/lua_ui.h"
#include "core/config.h"
#include "core/game_dir.h"
#include "core/log.h"
#include "platform/open_folder.h"
#include "platform/paths.h"
#include "ui/lua/lua_host.h"

namespace hailstone::app {

struct UiDemo::ModernChat {
    GameDir game; // A fresh preview uses the installed client's default chat colors.
    UiHost host;
    UiApi api{host};
    ui::lua::LuaUi lua;
    bool verified = false;

    ModernChat(ui::ChatView& view, const std::filesystem::path& scratch)
        : lua(platform::ui_packs_dir() / "modern", scratch / "modern.txt") {
        std::filesystem::create_directories(scratch);
        const auto layout = scratch / "UI_Demo.ini";
        std::ofstream(layout) << "[ChatManager]\nChatWindow0_Name=Main Chat\n";
        host.game = &game;
        host.chat = &view;
        host.in_game = host.ui_ready = true;
        host.game_state = 5;
        host.layout_setting = [this](std::string_view key) { return lua.setting(key); };
        host.set_layout_setting = [this](std::string_view key, std::string_view value) { lua.set_setting(key, value); };
        host.submit_chat = [&view](const std::string& line) {
            view.add(ui::ChatChannel::Say, std::format("You say, '{}'", line));
        };
        api.load_layout({}, layout, {});
        api.show("Chat1", true);
        lua.set_entry_scripts({"chat.lua"});
        lua.set_setting("hud.size.Chat1", "660,390");
        lua.set_setting("hud.pos.Chat1", "16,314");
        lua.add_ui_api([this](lua_State* state) { api.register_functions(state); });
    }

    void draw() {
        api.begin_frame();
        lua.frame();
        if (verified) return;
        const auto status = lua.load_status();
        if (!status.errors.empty()) throw std::runtime_error(status.errors.front());
        if (!api.replaces("Chat1")) throw std::runtime_error("The Modern Lua chat preview did not open Chat1.");
        log::info("ui demo: Modern Lua Chat1");
        verified = true;
    }
};

UiDemo::~UiDemo() {
    modern_chat_.reset();
    std::error_code ec;
    if (!scratch_.empty()) std::filesystem::remove_all(scratch_, ec);
}

bool parse_demo_screen(std::string_view name, DemoScreen& out) {
    struct Entry { std::string_view name; DemoScreen screen; };
    constexpr Entry kNames[] = {
        {"login", DemoScreen::Login},          {"servers", DemoScreen::Servers},
        {"chars", DemoScreen::Characters},     {"nochars", DemoScreen::NoCharacters},
        {"loading", DemoScreen::Loading},      {"game", DemoScreen::InGame},
        {"items", DemoScreen::Items},
        {"client-chat", DemoScreen::ClientChat}, {"client-chat-expanded", DemoScreen::ClientChatExpanded},
    };
    for (const auto& e : kNames)
        if (e.name == name) { out = e.screen; return true; }
    return false;
}

UiDemo::UiDemo(DemoScreen start) : screen_(start) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    scratch_ = fs::temp_directory_path(ec) / ("hailstone-ui-demo-" + std::to_string(stamp));
    const fs::path servers = scratch_ / "servers";
    for (const ServerProfile& p : list_profiles()) {
        fs::create_directories(profile_dir(p.id, servers), ec);
        fs::copy_file(profile_ini(p.id), profile_ini(p.id, servers), fs::copy_options::skip_existing, ec);
    }
    config_.load(default_config_path());
    ui::load_login_settings(login_, config_, servers);
    login_.status = {"Demo: any username and password log in. Changes to the servers are not kept.", false};

    servers_.servers = {
        {"Dev Server", "Up", true},
        {"Test Server (Locked)", "Locked", false},
        {"Old Server", "Down", false},
    };
    favorite_world(-1); // (in the order the screen lists worlds in)
    servers_.selected = 0;

    chars_.server_name = "Dev Server";
    chars_.characters = {
        {"Gennro", 65, 128, {5, 2, 12}, "The Overthere"},
        {"Lightbringer", 12, 1, {3}, "North Qeynos"},
        {"Stumpy", 50, 8, {1, 9}, "Kaladim"},
        {"Tinkerbell", 1, 12, {13}, "Ak'Anon"},
        {"Purrwhisker", 35, 130, {15}, "Shar Vahl"},
        {"Scaleface", 70, 522, {14, 11, 6}, "Plane of Knowledge"},
    };
    chars_.characters[1].can_go_home = true; // the demo's Return Home
    chars_.characters[1].home_question = "Demo: Lightbringer would go to the home city.";
    chars_.selected = 0;
    no_chars_.server_name = "Dev Server";

    loading_ = {"Entering World", "Loading zone qeynos2", 0.35f};

    game_.hud.connection = "Connected";
    game_.chat.add(ui::ChatChannel::System, "Welcome to EverQuest!");
    game_.chat.add(ui::ChatChannel::System, "You have entered Qeynos Hills.");
    game_.chat.add(ui::ChatChannel::Say, "Guard Weaver says, 'Hail, Gennro. Watch yourself near the gnolls.'");
    game_.chat.add(ui::ChatChannel::Tell, "Lightbringer tells you, 'need a port to PoK?'");
    game_.chat.add(ui::ChatChannel::Group, "Stumpy tells the group, 'pulling'");
    game_.chat.add(ui::ChatChannel::Guild, "Tinkerbell tells the guild, 'grats!'");
    game_.chat.add(ui::ChatChannel::OOC, "Purrwhisker says out of character, 'LFG'");
    game_.chat.add(ui::ChatChannel::Auction, "Scaleface auctions, 'WTS Fine Steel Long Sword 5pp'");
    game_.chat.add(ui::ChatChannel::System,
                   "Welcome to the server! This is a long message of the day that wraps onto several lines, so the "
                   "chat window has to place lines of different heights: rules, events and links all fit here.");
    game_.chat.add(ui::ChatChannel::Say, "Guard Weaver says, 'The line after a wrapped one must still show.'");
    game_.chat.add(ui::ChatChannel::Shout, "A gnoll shouts, 'Die, human!'");
    game_.chat.add(ui::ChatChannel::Combat, "You slash a gnoll pup for 12 points of damage.");
    game_.chat.add(ui::ChatChannel::Spell, "You feel the spirit of wolf enter you.");
    game_.chat.add(ui::ChatChannel::Error, "You cannot see your target.");
    if (start == DemoScreen::ClientChat || start == DemoScreen::ClientChatExpanded) {
        game_.chat.lines.clear();
        game_.chat.add(ui::ChatChannel::System, "-- Demo server - Welcome to the Hailstone chat preview --", 0xe5e9ef);
        game_.chat.add(ui::ChatChannel::System, "You have entered The Skyfire Mountains.", 0xe5e9ef);
        game_.chat.add_client_messages({
            "[Lua] bagcheck stopped: it may not run here",
            "[Plugin] groupstatus: it may not run here",
            "[Plugin] groupstatus unloaded",
            "[Plugin] lootnotes: it may not run here",
            "[Plugin] lootnotes unloaded",
            "[Plugin] targetpanel: it may not run here",
            "[Plugin] targetpanel unloaded",
            "[Lua] running bagcheck (read,commands,ui)",
            "[Plugin] groupstatus loaded (read,ui)",
            "[Plugin] lootnotes loaded (events,ui)",
            "[Plugin] targetpanel loaded (read,ui)",
            "[Plugin] sessionlog loaded (events)",
        });
        if (start == DemoScreen::ClientChatExpanded)
            game_.chat.toggle_chat_group(game_.chat.lines.back());
        game_.chat.add_chat_group("Inventory Check", {
            "Checked four bags.",
            "Seven slots are free.",
            "One item is ready to parcel.",
        });
        modern_chat_ = std::make_unique<ModernChat>(game_.chat, scratch_);
    }
    game_.spawns.spawns = {
        {101, "a gnoll pup", 2, 54.0f},     {102, "Guard Weaver", 30, 120.0f},
        {103, "a rabid wolf", 6, 410.0f},   {104, "a large rat", 1, 22.0f},
        {105, "Lightbringer", 12, 300.0f},  {106, "a decaying skeleton", 3, 820.0f},
        {107, "Baobob Miller", 5, 640.0f},  {108, "a fire beetle", 4, 960.0f},
    };
}

// Fake items for the items screen (icons are stock icon numbers).
void UiDemo::init_items() {
    auto make = [](std::uint32_t id, const char* name, std::uint32_t icon) {
        net::Item it;
        it.id = id;
        it.name = it.lore = name;
        it.icon = icon;
        it.no_drop = 255;
        it.no_rent = 1;
        it.classes = 0xffff;
        it.races = 0xffff;
        it.size = 2;
        it.weight = 10;
        return it;
    };
    net::Item sword = make(5019, "Fine Steel Long Sword", 580);
    sword.equip_slots = (1u << net::kSlotPrimary) | (1u << net::kSlotSecondary);
    sword.damage = 8;
    sword.delay = 30;
    sword.astr = 3;
    sword.heroic_str = 2;
    sword.hp = 10;
    sword.value = 5000;
    sword.magic = 1;
    sword.effects[net::kEffectProc].effect = 2;
    sword.effects[net::kEffectProc].name = "Lifetap";
    items_inv_.put(net::ItemSlot::possessions(net::kSlotPrimary), sword);
    net::Item cap = make(1001, "Cloth Cap", 640);
    cap.equip_slots = 1u << net::kSlotHead;
    cap.ac = 1;
    items_inv_.put(net::ItemSlot::possessions(net::kSlotHead), cap);
    net::Item bag = make(17005, "Backpack", 539);
    bag.item_class = 1;
    bag.bag_slots = 8;
    bag.bag_size = 3;
    bag.weight_reduction = 0;
    items_inv_.put(net::ItemSlot::possessions(net::kSlotGeneral1), bag);
    net::Item bread = make(9991, "Bread Cakes", 1021);
    bread.max_stack = 20;
    bread.stack_count = 5;
    items_inv_.put(net::ItemSlot::possessions(net::kSlotGeneral1, 0), bread);
    net::Item milk = make(9990, "Skin of Milk", 717);
    milk.max_stack = 20;
    milk.stack_count = 12;
    items_inv_.put(net::ItemSlot::possessions(net::kSlotGeneral1, 3), milk);
    items_inv_.put(net::ItemSlot::possessions(net::kSlotGeneral1 + 2), make(18707, "Recruitment Flyer", 864));
    net::Item whiskers = make(13071, "Rat Whiskers", 794);
    whiskers.max_stack = 20;
    whiskers.stack_count = 6;
    items_inv_.put(net::ItemSlot::possessions(net::kSlotCursor), whiskers);
    items_loot_[23] = make(13915, "Gnoll Fang", 1047);
    items_loot_[24] = make(13916, "Ruined Gnoll Pelt", 1009);
    items_loot_[25] = make(5013, "Rusty Short Sword", 580);
    items_state_.show_inventory = true;
    items_state_.open_bags.insert(net::kSlotGeneral1);
    ui::show_item_info(items_state_, sword);
    game_.chat.add(ui::ChatChannel::System, "--You have looted a " + game::make_item_link(5019, "Fine Steel Long Sword", true) + ".--");
}

void UiDemo::favorite_world(int row) {
    auto& list = servers_.servers;
    if (row >= 0 && row < static_cast<int>(list.size()))
        list[static_cast<std::size_t>(row)].favorite = !list[static_cast<std::size_t>(row)].favorite;
    std::vector<std::string> names, favorites;
    for (const auto& s : list) {
        names.push_back(s.name);
        if (s.favorite) favorites.push_back(s.name);
    }
    const std::string picked = servers_.selected >= 0 && servers_.selected < static_cast<int>(list.size())
                                   ? list[static_cast<std::size_t>(servers_.selected)].name
                                   : std::string();
    std::vector<ui::ServerEntry> sorted;
    for (std::size_t i : ui::server_list_order(names, favorites)) {
        if (list[i].name == picked) servers_.selected = static_cast<int>(sorted.size());
        sorted.push_back(list[i]);
    }
    list = std::move(sorted);
}

void UiDemo::show(DemoScreen s) {
    screen_ = s;
    timer_ = 0.0f;
    loading_running_ = false;
}

void UiDemo::draw(const DemoFrameInput& in) {
    constexpr ImGuiKey kKeys[] = {ImGuiKey_F1, ImGuiKey_F2, ImGuiKey_F3, ImGuiKey_F4, ImGuiKey_F5, ImGuiKey_F6, ImGuiKey_F7};
    for (int i = 0; i < 7; ++i)
        if (ImGui::IsKeyPressed(kKeys[i], false)) show(static_cast<DemoScreen>(i));

    switch (screen_) {
    case DemoScreen::Login:
        if (login_.busy) {
            timer_ += in.dt;
            if (timer_ > 1.2f) {
                login_.busy = false;
                login_.status = {"Demo: any username and password log in.", false};
                show(DemoScreen::Servers);
            }
        }
        switch (ui::LoginAction action = ui::login_screen(login_)) {
        case ui::LoginAction::Submit:
        case ui::LoginAction::Save:
        case ui::LoginAction::Delete: {
            // The scratch copies only (scratch_).
            bool done = action == ui::LoginAction::Delete ? ui::delete_login_profile(login_, config_)
                                                          : ui::store_login_settings(login_, config_).has_value();
            if (!done) break; // the status says why
            if (action != ui::LoginAction::Submit) {
                login_.status = {action == ui::LoginAction::Save ? "Saved." : "Server deleted.", false};
                break;
            }
            login_.busy = true;
            login_.status = {std::format("Connecting to {}:{} ...", login_.host, login_.port), false};
            timer_ = 0.0f;
            break;
        }
        case ui::LoginAction::OpenAssets: platform::open_folder(ui::selected_assets_dir(login_)); break;
        case ui::LoginAction::UseGameFolder:
        case ui::LoginAction::BrowseGameFolder:
            login_.status = {"Demo: the game files are not changed here.", false};
            break;
        case ui::LoginAction::LoginBox:
        case ui::LoginAction::AccountsChanged:
        case ui::LoginAction::ForgetAccount:
        case ui::LoginAction::PlayCharacter:
        case ui::LoginAction::MusicChanged:
        case ui::LoginAction::ResumeToken:
            login_.status = {"Demo: the accounts are not changed here.", false};
            break;
        case ui::LoginAction::UpdateDownload:
        case ui::LoginAction::UpdateRestart:
        case ui::LoginAction::UpdateSkip:
        case ui::LoginAction::UpdateNotes:
        case ui::LoginAction::OpenProject:
            login_.status = {"Demo: there are no updates here.", false};
            break;
        case ui::LoginAction::AddonInspect:
        case ui::LoginAction::AddonInstall:
        case ui::LoginAction::AddonCancel:
        case ui::LoginAction::AddonUpdate:
        case ui::LoginAction::AddonRemove:
        case ui::LoginAction::AddonAuto:
        case ui::LoginAction::AddonFolder:
        case ui::LoginAction::AddonPage:
        case ui::LoginAction::AddonCheck:
        case ui::LoginAction::AddonBrowse:
        case ui::LoginAction::AddonStoreLook:
        case ui::LoginAction::AddonRevoke:
        case ui::LoginAction::AddonAskAgain:
        case ui::LoginAction::AddonSuggestLook:
        case ui::LoginAction::AddonSuggestLater:
        case ui::LoginAction::AddonSuggestNever:
        case ui::LoginAction::AddonStart:
        case ui::LoginAction::AddonEnabled:
            login_.status = {"Demo: add-ons are not installed here.", false};
            break;
        case ui::LoginAction::Quit: quit_ = true; break;
        case ui::LoginAction::None: break;
        }
        break;
    case DemoScreen::Servers:
        switch (ui::server_select_screen(servers_)) {
        case ui::ServerSelectAction::Play: show(DemoScreen::Characters); break;
        case ui::ServerSelectAction::Back: show(DemoScreen::Login); break;
        case ui::ServerSelectAction::Favorite: favorite_world(servers_.favorite_row); break;
        case ui::ServerSelectAction::None: break;
        }
        break;
    case DemoScreen::Characters:
    case DemoScreen::NoCharacters: {
        auto& view = screen_ == DemoScreen::Characters ? chars_ : no_chars_;
        switch (ui::character_select_screen(view)) {
        case ui::CharacterSelectAction::EnterWorld:
        case ui::CharacterSelectAction::ReturnHome:
            loading_.progress = 0.0f;
            show(DemoScreen::Loading);
            loading_running_ = true;
            break;
        case ui::CharacterSelectAction::Back: show(DemoScreen::Servers); break;
        case ui::CharacterSelectAction::Logout: show(DemoScreen::Login); break;
        case ui::CharacterSelectAction::Create:
        case ui::CharacterSelectAction::Delete:
        case ui::CharacterSelectAction::None: break;
        }
        break;
    }
    case DemoScreen::Loading:
        // Advances only after a fake load was started from character select.
        if (loading_running_) {
            timer_ += in.dt;
            loading_.progress = std::min(1.0f, timer_ / 2.0f);
            if (loading_.progress >= 1.0f) show(DemoScreen::InGame);
        }
        ui::loading_screen(loading_);
        break;
    case DemoScreen::ClientChat:
    case DemoScreen::ClientChatExpanded:
        modern_chat_->draw();
        return;
    case DemoScreen::InGame: {
        game_.hud.zone = in.zone.empty() ? std::string("(viewer, no zone)") : in.zone;
        game_.hud.x = in.x;
        game_.hud.y = in.y;
        game_.hud.z = in.z;
        game_.hud.heading = in.heading_deg;
        game_.hud.fps = ImGui::GetIO().Framerate;
        ui::InGameActions a = ui::in_game_overlay(game_);
        if (a.chat_submit) game_.chat.add(ui::ChatChannel::Say, std::format("You say, '{}'", *a.chat_submit));
        if (a.target_spawn) {
            for (const auto& s : game_.spawns.spawns)
                if (s.id == *a.target_spawn) game_.chat.add(ui::ChatChannel::System, "Targeted " + s.name + ".");
        }
        if (a.camp) {
            game_.chat.add(ui::ChatChannel::System, "It will take you about 30 seconds to prepare your camp.");
        }
        if (a.disconnect) show(DemoScreen::Login);
        break;
    }
    case DemoScreen::Items: {
        if (items_inv_.items().empty()) init_items();
        game_.hud.fps = ImGui::GetIO().Framerate;
        ui::InGameActions a = ui::in_game_overlay(game_);
        if (a.disconnect) show(DemoScreen::Login);
        if (game_.chat.clicked_link) {
            game_.chat.clicked_link.reset();
            if (const auto* sword = items_inv_.at(net::ItemSlot::possessions(net::kSlotPrimary)))
                ui::show_item_info(items_state_, *sword);
        }
        ui::ItemWindowsInput win;
        win.inventory = &items_inv_;
        win.coins = {12, 3, 4, 5};
        win.corpse_id = 77;
        win.corpse_name = "a gnoll pup's corpse";
        win.loot = &items_loot_;
        ui::ItemWindowsActions act = ui::item_windows(items_state_, win, item_icons_);
        if (act.click) {
            const net::ItemSlot cursor = net::ItemSlot::possessions(net::kSlotCursor);
            items_inv_.swap(*act.click, cursor);
        }
        if (act.loot) {
            auto it = items_loot_.find(act.loot->first);
            if (it != items_loot_.end()) {
                items_inv_.put(net::ItemSlot::possessions(net::kSlotCursor), it->second);
                items_loot_.erase(it);
            }
        }
        if (act.destroy) items_inv_.take(net::ItemSlot::possessions(net::kSlotCursor));
        break;
    }
    }

    // Screen switcher hint, bottom right.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 8.0f, vp->WorkPos.y + vp->WorkSize.y - 8.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("##hailstone_demo_hint", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
    ImGui::TextDisabled("UI demo  F1 login  F2 servers  F3 chars  F4 no chars  F5 loading  F6 in game  F7 items");
    ImGui::End();
}

} // namespace hailstone::app
