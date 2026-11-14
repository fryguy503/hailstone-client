#pragma once

// A GameConnection with a small world set up directly (no server), for the MacroQuest tests: the player Tester
// (level 50 cleric) at the origin, a rat, a named NPC, a group mate and four spells in spells_us.txt (Minor Healing,
// Selo's Accelerando, Holy Armor and a discipline, 4498, for the NMS and Triune plugins' timer tables), and two AA
// names in dbstr_us.txt (9001 Innate Strength, 9002 Mystical Shielding).

#include <filesystem>
#include <fstream>
#include <memory>
#include <functional>
#include <string>
#include <vector>

#include "core/game_dir.h"
#include "game/coords.h"
#include "net/game_connection.h"
#include "scratch_dir.h"

namespace hailstone::net {

struct GameConnectionTestAccess {
    static game::WorldState& world(GameConnection& c) { return c.world_; }
    static PlayerStatus& status(GameConnection& c) { return c.pstatus_; }
    static game::SpellState& spells(GameConnection& c) { return c.spells_; }
    static std::int32_t& drunkenness(GameConnection& c) { return c.drunkenness_; }
    static game::GroupState& group(GameConnection& c) { return c.group_; }
    static std::vector<std::pair<std::uint32_t, game::BuffSlot>>& pet_buffs(GameConnection& c) { return c.pet_buffs_; }
    static game::Inventory& inventory(GameConnection& c) { return c.inventory_; }
    static Coins& coins(GameConnection& c) { return c.coins_; }
    static std::optional<PlayerProfile>& profile(GameConnection& c) { return c.profile_; }
    static std::string& character(GameConnection& c) { return c.character_; }
    static bool& auto_attack(GameConnection& c) { return c.auto_attack_; }
    static ConnectionState& state(GameConnection& c) { return c.state_; }
    static std::optional<NewZone>& new_zone(GameConnection& c) { return c.new_zone_; }
    static std::vector<ZonePoint>& zone_points(GameConnection& c) { return c.zone_points_; }
    static void handle_zone(GameConnection& c, const AppPacket& p) { c.handle_zone(p); }
    static game::AltAdvState& alt_adv(GameConnection& c) { return c.alt_adv_; }
    static void update_usable_gems(GameConnection& c) { c.update_usable_gems(); }
    static void memorize_tick(GameConnection& c, std::int64_t now) { c.memorize_tick(now); }
    static void raise_hook_events(GameConnection& c) { c.raise_hook_events(); }
    static game::ClientOptions& client_options(GameConnection& c) { return c.client_options_; }
    static void open_chat_log(GameConnection& c, const std::string& name) { c.open_chat_log(name); }
    static void log_command(GameConnection& c, const std::string& args) { c.log_command(args); }
    static std::ofstream* chat_log_stream(GameConnection& c) { return c.chat_log_.get(); }
    static void chat(GameConnection& c, std::string text) { c.chat(ChatEvent::Kind::System, std::move(text)); }
    static std::string& safelock(GameConnection& c) { return c.safelock_; }
    static WorldTicket& ticket(GameConnection& c) { return c.ticket_; }
    static void set_server_rules(GameConnection& c, std::optional<plugin::Rules> r) {
        c.server_rules_ = std::move(r);
        ++c.server_rules_serial_;
    }
    static void update_server_rules(GameConnection& c) { c.update_server_rules(); }
    static void update_bulk_job(GameConnection& c, std::int64_t now) { c.update_bulk_job(now); }
    static auto& npc_checks(GameConnection& c) { return c.npc_checks_; }
    static void with_character(const GameConnection& c, const std::function<void(game::mechanics::Character&)>& fn) {
        c.with_character(fn);
    }
};

} // namespace hailstone::net

namespace hailstone::mq_test {

inline std::string spell_line(std::initializer_list<std::pair<int, std::string>> set) {
    std::vector<std::string> cols(237, "0");
    for (int i = 1; i <= 8; ++i) cols[static_cast<std::size_t>(i)].clear();
    for (const auto& [c, v] : set) cols[static_cast<std::size_t>(c)] = v;
    std::string line;
    for (std::size_t i = 0; i < cols.size(); ++i) line += (i ? "^" : "") + cols[i];
    return line;
}

struct World {
    std::filesystem::path dir;
    std::unique_ptr<GameDir> game;
    std::unique_ptr<net::GameConnection> conn;

    explicit World(const std::string& name, net::ConnectionOptions options = {}) {
        dir = test::unique_temp_path(name); // scratch_dir.h: concurrent test runs share the temp folder
        std::filesystem::create_directories(dir);
        {
            std::ofstream f(dir / "spells_us.txt");
            f << spell_line({{0, "200"}, {1, "Minor Healing"}, {9, "100"}, {13, "1500"}, {17, "0"}, {19, "10"},
                             {83, "1"}, {98, "5"}, {104, "255"}, {105, "1"}})
              << "\n";
            f << spell_line({{0, "717"}, {1, "Selo's Accelerando"}, {9, "0"}, {13, "3000"}, {17, "10"}, {83, "1"},
                             {98, "6"}, {154, "1"}})
              << "\n";
            f << spell_line({{0, "11"}, {1, "Holy Armor"}, {13, "2000"}, {17, "270"}, {19, "15"}, {83, "1"}, {98, "5"},
                             {105, "3"}})
              << "\n";
            f << spell_line({{0, "4498"}, {1, "Test Discipline"}, {17, "60"}, {83, "1"}}) << "\n";
            // Spell effects (86.., bases 20..): a mez (SPA 31) and a slow to 70% (SPA 11), detrimental.
            f << spell_line({{0, "3001"}, {1, "Test Mesmerize"}, {17, "8"}, {20, "1"}, {83, "0"}, {86, "31"}}) << "\n";
            f << spell_line({{0, "3002"}, {1, "Test Slow"}, {17, "30"}, {20, "70"}, {83, "0"}, {86, "11"}}) << "\n";
            // A disease counter (SPA 35, base 4) in the second effect slot, after a DoT.
            f << spell_line({{0, "3004"}, {1, "Test Plague"}, {17, "10"}, {20, "-10"}, {21, "4"}, {83, "0"}, {86, "0"},
                             {87, "35"}})
              << "\n";
            // A bard song (the Bard level, column 111, 1).
            f << spell_line({{0, "3373"}, {1, "Test Chant"}, {13, "1000"}, {17, "3"}, {83, "0"}, {111, "1"}}) << "\n";
        }
        // Two AA names (dbstr type 1), for the tests of hailstone.alt_abilities.
        std::ofstream(dir / "dbstr_us.txt") << "9001^1^Innate Strength^0^\n9002^1^Mystical Shielding^0^\n";
        game = std::make_unique<GameDir>(dir);
        options.servers_dir = dir / "servers"; // the character's files stay in the scratch folder
        conn = std::make_unique<net::GameConnection>(*game, options);
        using A = net::GameConnectionTestAccess;
        A::state(*conn) = net::ConnectionState::InZone;
        A::character(*conn) = "Tester";
        net::PlayerProfile prof;
        prof.class_ = 2;
        prof.race = 1;
        prof.level = 50;
        prof.aa_points = 7;
        A::profile(*conn) = prof;
        auto& ps = A::status(*conn);
        ps.hp = 750;
        ps.max_hp = 1000;
        ps.mana = 300;
        ps.max_mana = 1200;
        ps.endurance = 50;
        ps.max_endurance = 100;
        ps.level = 50;
        auto& w = A::world(*conn);
        w.zone_short_name = "qeynos2";
        w.zone_long_name = "North Qeynos";
        w.player_id = 1;
        add(1, "Tester", false, 50, {0, 0, 0});
        add(10, "a_rat01", true, 2, game::server_to_world({30, 40, 0})); // 50 away
        add(11, "Fippy_Darkpaw000", true, 8, game::server_to_world({0, 100, 2}));
        add(12, "Groupie", false, 48, game::server_to_world({0, -20, 0}));
        add(13, "a_rat02", true, 3, game::server_to_world({200, 0, 0}));
        w.spawns[13].is_corpse = true;
        w.target_id = 10;
        auto& sp = A::spells(*conn);
        sp.gems[0].spell = 200;
        sp.gems[1].spell = 11;
        sp.buffs[0].spell = 11;
        sp.buffs[0].tics = 10;
        sp.buffs[0].received_ms = net::GameConnection::now_ms();
        sp.buffs[3].spell = 717;
        sp.buffs[3].tics = 2;
        sp.buffs[3].received_ms = net::GameConnection::now_ms();
        auto& grp = A::group(*conn);
        grp.active = true;
        grp.leader = "Groupie";
        grp.slots[0].name = "Groupie";
        grp.slots[0].level = 48;
        grp.slots[0].main_tank = true;
        grp.slots[1].name = "Tester";
        A::coins(*conn) = {12, 3, 4, 5};
        A::auto_attack(*conn) = true;
        net::Item potion;
        potion.name = "Healing Potion";
        potion.id = 5001;
        potion.stack_count = 5;
        potion.max_stack = 20;
        A::inventory(*conn).put(net::ItemSlot::possessions(net::kSlotGeneral1), potion);
        net::Item bag;
        bag.name = "Small Bag";
        bag.id = 17005;
        bag.item_class = 1;
        bag.bag_slots = 4;
        A::inventory(*conn).put(net::ItemSlot::possessions(net::kSlotGeneral1 + 1), bag);
        net::Item gem;
        gem.name = "Healing Potion";
        gem.id = 5001;
        gem.stack_count = 2;
        gem.max_stack = 20;
        A::inventory(*conn).put(net::ItemSlot::possessions(net::kSlotGeneral1 + 1, 0), gem);
    }
    ~World() {
        conn.reset();
        game.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    void add(std::uint32_t id, const std::string& name, bool npc, int level, glm::vec3 pos) {
        auto& w = net::GameConnectionTestAccess::world(*conn);
        if (!w.spawns.contains(id)) ++w.spawn_set_serial; // as GameConnection::apply_spawn
        auto& s = w.spawns[id];
        s.id = id;
        s.name = name;
        s.is_npc = npc;
        s.level = static_cast<std::uint8_t>(level);
        s.position = pos;
        s.class_id = npc ? 1 : 2;
        s.race = 1;
        s.hp_percent = npc ? 80 : 100;
    }
};

} // namespace hailstone::mq_test
