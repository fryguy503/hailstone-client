#pragma once

// playprobe's quest runner (included by tools/playprobe.cpp):
//   --quest FILE     runs the steps in FILE, one per line, and prints "QUEST PASS" / "QUEST FAIL" for each check
//                    and a summary. Meant for a GM character on a test server: it moves with #goto, kills
//                    with #damage and zones with #zone. Coordinates are the server's (#loc order x y z).
//
// Steps ('#' at the start of a line is a comment):
//   say TEXT            says TEXT (a GM command too: "say #level 100")
//   wait SECONDS
//   goto X Y Z          #goto, then waits for the move
//   zone SHORTNAME      #zone, then waits for the new zone (passes at once when already there)
//   near NAME           moves to the nearest living spawn whose name contains NAME (or one of a|b|c) and targets it
//   hail NAME           near NAME, then hails it
//   ask NAME TEXT       near NAME, then says TEXT to it
//   accept TITLE        accepts the offered task whose title contains TITLE
//   kill NAME [N]       N times: near NAME, #damage until it dies
//   open NAME           near NAME, then /open (an adventure chest NPC)
//   loot ITEMID         loots ITEMID from the nearest corpse
//   give NAME ITEMID    near NAME, then trades it ITEMID from the inventory
//   click ITEMID        uses ITEMID from the inventory
//   pickup              moves to the nearest ground spawn and picks it up (then auto-inventories it)
//   journal             prints the task journal
//   expect-active TITLE / expect-done TITLE / expect-not-active TITLE
//   expect-element TITLE N   element N (activity id) of the active task is done
//   expect-item ITEMID / expect-no-item ITEMID / expect-chat TEXT (any chat line since the previous expect-chat)
//   cancel TITLE        abandons the active task
//   stop-on-fail        stops the run at the first failed check
//   bandolier N         activates bandolier set N (the client's swap), then prints the possessions
//   dump-inventory      prints the possessions (worn, general, cursor, and the bags there)
//   snapshot            keeps the possessions as the client sees them now
//   expect-snapshot     the possessions are what the snapshot kept (after a zone, the server's own inventory)
//   expect-at SLOT SUB ITEMID   possessions slot SLOT (bag slot SUB, -1 the slot itself) holds ITEMID (0: empty)

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <glm/geometric.hpp>

#include "game/coords.h"
#include "game/world_state.h"
#include "game/zoning.h"
#include "net/game_connection.h"

namespace hailstone::probe {

class QuestProbe {
public:
    using Clock = std::chrono::steady_clock;

    bool option(const char* name, const char* value) {
        if (std::strcmp(name, "--quest") != 0) return false;
        std::ifstream in(value);
        if (!in) {
            std::printf("  QUEST: cannot read %s\n", value);
            done_ = true;
            return true;
        }
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            std::size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos || line[start] == '#') continue;
            lines_.push_back(line.substr(start));
        }
        active_ = true;
        return true;
    }

    bool active() const { return active_; }
    bool busy() const { return active_ && !done_; }

    void chat(const std::string& text) {
        if (active_) chat_.push_back(lower(text));
    }

    void zone_in(net::GameConnection& conn) {
        if (!active_) return;
        if (const auto* me = conn.world().player()) pos_ = me->position;
        if (!handler_set_) {
            handler_set_ = true;
            // A request for the zone we are in (#goto) moves us without zoning, as DoTeleportB does; another zone
            // zones (MoveToZone).
            conn.set_teleport_handler([this, &conn](const game::TeleportBArgs& a) {
                if ((static_cast<std::uint32_t>(a.zone) & 0xffffu) == (conn.current_zone_dword() & 0xffffu)) {
                    pos_ = glm::vec3(a.x, a.y, a.z);
                    conn.set_player_motion(pos_, a.heading < 513.0f ? a.heading : 0.0f, glm::vec3(0.0f), false);
                    conn.send_position_now();
                    moved_ = true;
                    return;
                }
                game::MoveToZoneCall m;
                m.zone = a.zone;
                m.x = a.x;
                m.y = a.y;
                m.z = a.z;
                m.heading = static_cast<std::int32_t>(a.heading);
                conn.move_to_zone(m);
            });
        }
        zoned_ = true;
        since_ = Clock::now();
    }

    void step(net::GameConnection& conn) {
        if (!active_ || done_) return;
        auto now = Clock::now();
        if (now < wait_until_) return;
        if (index_ >= lines_.size()) {
            finish_run();
            return;
        }
        std::istringstream words(lines_[index_]);
        std::string cmd;
        words >> cmd;
        std::string rest;
        std::getline(words >> std::ws, rest);
        if (phase_ == 0) {
            std::printf("  QUEST [%zu] %s\n", index_ + 1, lines_[index_].c_str());
            started_ = now;
        }
        if (now - started_ > std::chrono::seconds(cmd == "zone" ? 60 : cmd == "kill" ? 30 * std::max(1, count_arg(rest)) : 20)) {
            check(false, "timed out");
            next();
            return;
        }
        run(conn, cmd, rest, now);
    }

    void finish(const net::GameConnection& conn) {
        if (!active_) return;
        if (!done_) {
            std::printf("  QUEST: stopped at step %zu of %zu\n", index_ + 1, lines_.size());
            print_journal(conn);
        }
        std::printf("QUEST SUMMARY: %d passed, %d failed, %zu of %zu steps run\n", passed_, failed_, index_,
                    lines_.size());
    }

    bool all_passed() const { return failed_ == 0 && index_ >= lines_.size(); }

private:
    void run(net::GameConnection& conn, const std::string& cmd, const std::string& rest, Clock::time_point now) {
        const auto& w = conn.world();
        if (cmd == "say") {
            conn.say(rest);
            pause(now, 1500);
        } else if (cmd == "wait") {
            pause(now, static_cast<int>(std::strtof(rest.c_str(), nullptr) * 1000.0f));
        } else if (cmd == "stop-on-fail") {
            stop_on_fail_ = true;
            next();
        } else if (cmd == "goto") {
            if (phase_ == 0) {
                moved_ = false;
                conn.say("#goto " + rest);
                phase_ = 1;
            } else if (moved_) {
                pause(now, 1000);
            }
        } else if (cmd == "zone") {
            if (phase_ == 0 && w.zone_short_name == rest) {
                // #zone to the zone we are in moves us to its safe point without a zone-in, so there is nothing to
                // wait for; a script starts from a known zone with it whatever zone it began in.
                check(true, "already in " + rest);
                next();
            } else if (phase_ == 0) {
                zoned_ = false;
                conn.say("#zone " + rest);
                phase_ = 1;
            } else if (zoned_ && w.zone_short_name == rest && now - since_ > std::chrono::seconds(4)) {
                check(true, "in " + rest);
                next();
            }
        } else if (cmd == "near" || cmd == "hail" || cmd == "ask" || cmd == "open" || cmd == "give") {
            std::string name = rest, text;
            std::uint32_t item = 0;
            if (cmd == "ask" || cmd == "give") {
                // The NPC name is the first word (underscores for spaces); the rest is the text or the item id.
                std::size_t sp = rest.find(' ');
                name = rest.substr(0, sp);
                text = sp == std::string::npos ? "" : rest.substr(sp + 1);
                if (cmd == "give") item = static_cast<std::uint32_t>(std::strtoul(text.c_str(), nullptr, 10));
            }
            if (phase_ == 0) {
                if (!go_near(conn, name)) {
                    check(false, "nobody named \"" + name + "\" here");
                    next();
                    return;
                }
                phase_ = 1;
                return;
            }
            if (phase_ == 1) {
                if (!moved_) return;
                conn.set_target(target_);
                phase_ = 2;
                wait_until_ = now + std::chrono::milliseconds(800);
                return;
            }
            if (cmd == "near") {
                next();
            } else if (cmd == "hail") {
                conn.hail();
                pause(now, 2000);
            } else if (cmd == "ask") {
                conn.say(text);
                pause(now, 2000);
            } else if (cmd == "open") {
                conn.send_command("/open");
                pause(now, 2500);
            } else if (phase_ == 2 && conn.inventory().cursor()) {
                std::printf("  QUEST: cursor holds %s; auto inventory\n", conn.inventory().cursor()->name.c_str());
                conn.auto_inventory();
                wait_until_ = now + std::chrono::milliseconds(1200);
            } else if (phase_ == 2) {
                if (!pick_up(conn, item)) {
                    check(false, "no item " + text + " to give");
                    next();
                    return;
                }
                phase_ = 3;
                wait_until_ = now + std::chrono::milliseconds(700);
            } else if (phase_ == 3) {
                conn.give_to(target_);
                phase_ = 4;
                wait_until_ = now + std::chrono::milliseconds(1200);
            } else if (phase_ == 4) {
                conn.trade_drop_cursor();
                phase_ = 5;
                wait_until_ = now + std::chrono::milliseconds(700);
            } else if (phase_ == 5) {
                conn.trade_accept();
                pause(now, 3000);
            }
        } else if (cmd == "accept") {
            const auto& offers = conn.tasks().offers;
            if (!offers) return; // wait for the window (the step times out)
            for (const auto& t : offers->offers)
                if (contains(t.title, rest)) {
                    std::printf("  QUEST: accepting task %u '%s'\n", t.task_id, t.title.c_str());
                    conn.accept_task(t.task_id);
                    pause(now, 2500);
                    return;
                }
            std::string seen;
            for (const auto& t : offers->offers) seen += " '" + t.title + "'";
            check(false, "no offer named \"" + rest + "\"; offered:" + seen);
            conn.decline_tasks();
            next();
        } else if (cmd == "kill") {
            kill(conn, rest, now);
        } else if (cmd == "loot") {
            loot(conn, static_cast<std::uint32_t>(std::strtoul(rest.c_str(), nullptr, 10)), now);
        } else if (cmd == "click" && phase_ == 0 && conn.inventory().cursor()) {
            // An item is used from the inventory, never the cursor ("Invalid inventory slot for using effects").
            std::printf("  QUEST: cursor holds %s; auto inventory\n", conn.inventory().cursor()->name.c_str());
            conn.auto_inventory();
            phase_ = 1;
            wait_until_ = now + std::chrono::milliseconds(1200);
        } else if (cmd == "click") {
            auto slot = find_item(conn, static_cast<std::uint32_t>(std::strtoul(rest.c_str(), nullptr, 10)));
            if (!slot) {
                check(false, "no item " + rest);
                next();
                return;
            }
            std::printf("  QUEST: use_item -> %d\n", conn.use_item(*slot));
            pause(now, 4000);
        } else if (cmd == "pickup") {
            if (phase_ == 0) {
                const game::GroundObject* best = nullptr;
                for (const auto& o : conn.ground_objects())
                    if (o.type <= 1 && (!best || glm::distance(o.position, pos_) < glm::distance(best->position, pos_))) best = &o;
                if (!best) {
                    check(false, "no ground spawn here");
                    next();
                    return;
                }
                object_ = best->drop_id;
                std::printf("  QUEST: ground spawn %u %s at world (%.0f %.0f %.0f)\n", best->drop_id, best->name.c_str(),
                            best->position.x, best->position.y, best->position.z);
                moved_ = false;
                conn.say(goto_text(best->position));
                phase_ = 1;
            } else if (phase_ == 1) {
                if (!moved_) return;
                std::printf("  QUEST: click_object -> %d\n", conn.click_object(object_));
                phase_ = 2;
                wait_until_ = now + std::chrono::seconds(2);
            } else {
                const net::Item* held = conn.inventory().cursor();
                bool ok = conn.auto_inventory();
                std::printf("  QUEST: cursor %s, auto_inventory -> %d\n", held ? held->name.c_str() : "empty", ok);
                pause(now, 1500);
            }
        } else if (cmd == "journal") {
            print_journal(conn);
            next();
        } else if (cmd == "expect-active" || cmd == "expect-not-active") {
            bool on = find_task(conn, rest) != nullptr;
            check(cmd == "expect-active" ? on : !on, rest + (on ? " is active" : " is not active"));
            if (failed_last_) print_journal(conn);
            next();
        } else if (cmd == "expect-done") {
            bool found = false;
            for (const auto& c : conn.tasks().completed) found |= contains(c.title, rest);
            check(found, rest + (found ? " is completed" : " is not in the completed list"));
            if (failed_last_) print_journal(conn);
            next();
        } else if (cmd == "expect-element") {
            std::size_t sp = rest.rfind(' ');
            std::string title = rest.substr(0, sp);
            auto id = static_cast<std::uint32_t>(std::strtoul(rest.c_str() + sp + 1, nullptr, 10));
            const game::Task* t = find_task(conn, title);
            auto e = t ? t->elements.find(id) : decltype(t->elements.end()){};
            bool ok = t && e != t->elements.end() && e->second.done();
            check(ok, title + " element " + std::to_string(id) +
                          (!t ? ": task not active"
                              : e == t->elements.end() ? ": no such element"
                                                       : " " + std::to_string(e->second.done_count) + "/" +
                                                             std::to_string(e->second.goal_count)));
            if (failed_last_) print_journal(conn);
            next();
        } else if (cmd == "expect-item" || cmd == "expect-no-item") {
            bool have = find_item(conn, static_cast<std::uint32_t>(std::strtoul(rest.c_str(), nullptr, 10))).has_value();
            check(cmd == "expect-item" ? have : !have, "item " + rest + (have ? " in the inventory" : " not in the inventory"));
            next();
        } else if (cmd == "expect-chat") {
            std::string want = lower(rest);
            bool ok = std::any_of(chat_.begin(), chat_.end(), [&](const std::string& c) { return c.find(want) != std::string::npos; });
            chat_.clear();
            check(ok, "chat \"" + rest + "\"");
            next();
        } else if (cmd == "bandolier") {
            const bool ok = conn.bandolier_activate(std::atoi(rest.c_str()));
            check(ok, "bandolier set " + rest + (ok ? " swapped" : " not swapped"));
            print_possessions(conn);
            pause(now, 1500);
        } else if (cmd == "dump-inventory") {
            print_possessions(conn);
            next();
        } else if (cmd == "snapshot") {
            snapshot_ = possessions(conn);
            std::printf("  QUEST: snapshot of %zu slots\n", snapshot_.size());
            next();
        } else if (cmd == "expect-snapshot") {
            const auto have = possessions(conn);
            for (const auto& l : snapshot_)
                if (std::find(have.begin(), have.end(), l) == have.end()) std::printf("  QUEST: was   %s\n", l.c_str());
            for (const auto& l : have)
                if (std::find(snapshot_.begin(), snapshot_.end(), l) == snapshot_.end()) std::printf("  QUEST: now   %s\n", l.c_str());
            check(have == snapshot_, "possessions match the snapshot (" + std::to_string(have.size()) + " slots)");
            next();
        } else if (cmd == "expect-at") {
            std::istringstream in(rest);
            int slot = 0, sub = -1;
            std::uint32_t id = 0;
            in >> slot >> sub >> id;
            const net::Item* it = conn.inventory().at(
                net::ItemSlot::possessions(static_cast<std::int16_t>(slot), static_cast<std::int16_t>(sub)));
            const std::uint32_t got = it ? it->id : 0;
            check(got == id, "slot " + std::to_string(slot) + "/" + std::to_string(sub) + " holds " + std::to_string(got) +
                                 (got == id ? "" : ", wanted " + std::to_string(id)));
            next();
        } else if (cmd == "cancel") {
            if (const game::Task* t = find_task(conn, rest)) conn.cancel_task(t->type, t->sequence);
            pause(now, 2000);
        } else {
            check(false, "unknown step \"" + cmd + "\"");
            next();
        }
    }

    void kill(net::GameConnection& conn, const std::string& rest, Clock::time_point now) {
        std::istringstream in(rest);
        std::string name;
        in >> name;
        int want = count_arg(rest);
        const auto& w = conn.world();
        if (phase_ == 0) {
            kills_ = 0;
            phase_ = 1;
        }
        if (phase_ == 1) {
            if (kills_ >= want) {
                check(true, "killed " + std::to_string(kills_) + " " + name);
                pause(now, 1500);
                return;
            }
            if (!go_near(conn, name)) {
                check(false, "no living \"" + name + "\" left after " + std::to_string(kills_));
                next();
                return;
            }
            phase_ = 2;
        } else if (phase_ == 2) {
            if (!moved_) return;
            conn.set_target(target_);
            phase_ = 3;
            wait_until_ = now + std::chrono::milliseconds(600);
        } else if (phase_ == 3) {
            auto it = w.spawns.find(target_);
            if (it == w.spawns.end() || it->second.is_corpse) {
                ++kills_;
                phase_ = 1;
                wait_until_ = now + std::chrono::milliseconds(800);
                return;
            }
            conn.say("#damage 2000000000");
            wait_until_ = now + std::chrono::milliseconds(700);
        }
    }

    void loot(net::GameConnection& conn, std::uint32_t item, Clock::time_point now) {
        const auto& w = conn.world();
        if (phase_ == 0 && conn.inventory().cursor()) {
            // The server refuses to loot onto a full cursor ("You may not loot an item while you have an item on your
            // cursor"): put away what is there first.
            std::printf("  QUEST: cursor holds %s; auto inventory\n", conn.inventory().cursor()->name.c_str());
            conn.auto_inventory();
            wait_until_ = now + std::chrono::milliseconds(1200);
            return;
        }
        if (phase_ == 0) {
            std::uint32_t corpse = 0;
            float best = 1e30f;
            for (const auto& [id, s] : w.spawns) {
                if (!s.is_corpse || !s.is_npc) continue;
                float d = glm::distance(s.position, pos_);
                if (d < best) { best = d; corpse = id; }
            }
            if (!corpse || best > 60.0f) {
                check(false, "no corpse near");
                next();
                return;
            }
            corpse_ = corpse;
            conn.loot(corpse);
            phase_ = 1;
            wait_until_ = now + std::chrono::milliseconds(1500);
        } else if (phase_ == 1) {
            const auto& s = conn.loot_session();
            if (!s || !s->open || !s->complete) return; // every item first (OP_LootComplete)
            for (const auto& [slot, it] : s->items)
                if (it.id == item) {
                    std::printf("  QUEST: looting %s from corpse slot %d\n", it.name.c_str(), slot);
                    conn.loot_item(slot, true);
                    phase_ = 2;
                    wait_until_ = now + std::chrono::milliseconds(2500);
                    return;
                }
            check(false, "item " + std::to_string(item) + " is not on the corpse (" + std::to_string(s->items.size()) + " items)");
            conn.end_loot();
            next();
        } else if (phase_ == 2 && conn.confirmation() && conn.confirmation()->id == net::kConfirmLootNoDrop) {
            // A NO DROP item asks first (LootNoDrop); a player says yes.
            std::printf("  QUEST: yes to \"%s\"\n", conn.confirmation()->text.c_str());
            conn.answer_confirmation(true);
            phase_ = 3;
            wait_until_ = now + std::chrono::milliseconds(2500);
        } else {
            conn.end_loot();
            check(find_item(conn, item).has_value(), "looted " + std::to_string(item));
            pause(now, 1000);
        }
    }

    // #goto beside the nearest living spawn named NAME (case-insensitive; '_' or ' '); remembers it in target_.
    bool go_near(net::GameConnection& conn, const std::string& name) {
        const auto& w = conn.world();
        std::string want = lower(name);
        std::replace(want.begin(), want.end(), ' ', '_');
        float best = 1e30f;
        target_ = 0;
        for (const auto& [id, s] : w.spawns) {
            if (s.is_corpse || id == w.player_id) continue;
            std::string n = lower(s.name);
            std::replace(n.begin(), n.end(), ' ', '_');
            bool match = false;
            for (std::size_t from = 0; from <= want.size();) {
                std::size_t bar = want.find('|', from);
                if (bar == std::string::npos) bar = want.size();
                match |= bar > from && n.find(want.substr(from, bar - from)) != std::string::npos;
                from = bar + 1;
            }
            if (!match) continue;
            float d = glm::distance(s.position, pos_);
            if (d < best) { best = d; target_ = id; }
        }
        if (!target_) return false;
        const auto& s = w.spawns.at(target_);
        moved_ = false;
        conn.say(goto_text(s.position + glm::vec3(3.0f, 0.0f, 0.0f)));
        return true;
    }

    // world (x, y, z) -> "#goto" with the server's x y z.
    static std::string goto_text(const glm::vec3& world) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "#goto %.1f %.1f %.1f", static_cast<double>(world.y), static_cast<double>(world.x),
                      static_cast<double>(world.z));
        return buf;
    }

    static std::optional<net::ItemSlot> find_item(const net::GameConnection& conn, std::uint32_t id) {
        for (const auto& [key, it] : conn.inventory().items()) {
            if (key.first != net::kInvPossessions) continue;
            if (it.id == id) return net::ItemSlot::possessions(key.second);
            for (const auto& b : it.contents)
                if (b.item.id == id)
                    return net::ItemSlot::possessions(key.second, static_cast<std::int16_t>(b.index));
        }
        return std::nullopt;
    }

    bool pick_up(net::GameConnection& conn, std::uint32_t id) {
        auto slot = find_item(conn, id);
        if (!slot) return false;
        conn.click_slot(*slot);
        return true;
    }

    static const game::Task* find_task(const net::GameConnection& conn, const std::string& title) {
        for (const auto& t : conn.tasks().tasks)
            if (contains(t.title, title)) return &t;
        return nullptr;
    }

    void print_journal(const net::GameConnection& conn) const {
        const auto& ts = conn.tasks();
        std::printf("  QUEST: %zu active tasks, %zu completed\n", ts.tasks.size(), ts.completed.size());
        for (const auto& t : ts.tasks) {
            std::printf("    task %u '%s'\n", t.task_id, t.title.c_str());
            for (const auto& [id, e] : t.elements)
                std::printf("      [%u] step %u %s %d/%d%s\n", id, e.step, e.hidden ? "? ? ?" : conn.task_element_text(e).c_str(),
                            e.done_count, e.goal_count, e.done() ? " done" : "");
        }
        for (const auto& c : ts.completed) std::printf("    completed %u '%s'\n", c.task_id, c.title.c_str());
    }

    // "slot/sub id count name" for every possessions item, in slot order.
    static std::vector<std::string> possessions(const net::GameConnection& conn) {
        std::vector<std::string> out;
        auto line = [&](int slot, int sub, const net::Item& it) {
            out.push_back(std::to_string(slot) + "/" + std::to_string(sub) + " " + std::to_string(it.id) + " x" +
                          std::to_string(it.stackable() ? it.stack_count : 1) + " " + it.name);
        };
        for (const auto& [key, top] : conn.inventory().items()) {
            if (key.first != net::kInvPossessions) continue;
            line(key.second, -1, top);
            std::vector<const net::BagItem*> in(top.contents.size());
            for (std::size_t i = 0; i < in.size(); ++i) in[i] = &top.contents[i];
            std::sort(in.begin(), in.end(), [](const net::BagItem* a, const net::BagItem* b) { return a->index < b->index; });
            for (const auto* b : in) line(key.second, static_cast<int>(b->index), b->item);
        }
        return out;
    }

    static void print_possessions(const net::GameConnection& conn) {
        for (const auto& l : possessions(conn)) std::printf("  QUEST:   %s\n", l.c_str());
    }

    void check(bool ok, const std::string& what) {
        std::printf("  QUEST %s [%zu]: %s\n", ok ? "PASS" : "FAIL", index_ + 1, what.c_str());
        (ok ? passed_ : failed_)++;
        failed_last_ = !ok;
        if (!ok && stop_on_fail_) {
            std::printf("  QUEST: stopping at the first failure\n");
            index_ = lines_.size();
        }
    }

    void pause(Clock::time_point now, int ms) {
        wait_until_ = now + std::chrono::milliseconds(ms);
        next();
    }

    void next() {
        if (index_ < lines_.size()) ++index_;
        phase_ = 0;
    }

    void finish_run() {
        done_ = true;
        std::printf("  QUEST: all %zu steps run\n", lines_.size());
    }

    static int count_arg(const std::string& rest) {
        std::size_t sp = rest.rfind(' ');
        if (sp == std::string::npos) return 1;
        int n = std::atoi(rest.c_str() + sp + 1);
        return n > 0 ? n : 1;
    }

    static std::string lower(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    static bool contains(const std::string& hay, const std::string& needle) {
        return lower(hay).find(lower(needle)) != std::string::npos;
    }

    std::vector<std::string> lines_;
    std::vector<std::string> chat_;
    std::vector<std::string> snapshot_;
    std::size_t index_ = 0;
    int phase_ = 0;
    int kills_ = 0;
    int passed_ = 0, failed_ = 0;
    bool active_ = false, done_ = false, handler_set_ = false, moved_ = false, zoned_ = false;
    bool stop_on_fail_ = false, failed_last_ = false;
    std::uint32_t target_ = 0, corpse_ = 0, object_ = 0;
    glm::vec3 pos_{0.0f};
    Clock::time_point wait_until_{}, started_{}, since_{};
};

} // namespace hailstone::probe
