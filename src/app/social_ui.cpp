#include "app/social_ui.h"

#include "game/eq_names.h"
#include "game/zone_names.h"
#include "mq/commands.h"
#include "net/game_connection.h"
#include "ui/lua/lua_host.h"

namespace hailstone::app {

namespace {

using Clock = std::chrono::steady_clock;

} // namespace

void SocialUi::draw(net::GameConnection& conn) {
    const auto& g = conn.guild();
    const auto& r = conn.raid();
    const auto& s = conn.social();
    auto now = Clock::now();

    // ---- guild
    guild_.in_guild = g.in_guild();
    guild_.guild_name = g.name_of(g.guild_id);
    guild_.motd = g.motd;
    guild_.motd_set_by = g.motd_set_by;
    guild_.members.clear();
    for (const auto& m : g.members) {
        ui::GuildMemberView v;
        v.name = m.name;
        v.level = static_cast<int>(m.level);
        v.klass = conn.guild_member_class(m.class_); // NMS: 1000 + mask as "War/Clr"
        v.rank = game::guild_rank_name(m.rank);
        if (m.online()) v.zone = game::zone_long_name(m.zone_id);
        v.note = m.public_note;
        guild_.members.push_back(std::move(v));
    }
    guild_.inviter = g.inviter;
    guild_.invite_guild = g.name_of(g.invite_guild);

    // ---- raid
    raid_.in_raid = r.active;
    raid_.locked = r.locked;
    raid_.leader = r.leader;
    raid_.motd = r.motd;
    raid_.inviter = r.inviter;
    raid_.groups.assign(13, {});
    for (const auto& m : r.members) {
        ui::RaidMemberView v;
        v.name = m.name;
        v.level = m.level;
        v.klass = m.class_ ? game::class_short_name(m.class_) : std::string();
        v.group_leader = m.group_leader;
        v.leader = m.name == r.leader;
        raid_.groups[m.group < 12 ? m.group : 12].push_back(std::move(v));
    }
    // CRaid_HandleInvite: "click the accept button in the raid window".
    if (!r.inviter.empty() && !raid_invited_) raid_.open = true;
    raid_invited_ = !r.inviter.empty();

    // ---- friends
    friends_.friends = s.friends;
    friends_.ignored.clear();
    for (const auto& i : s.ignored) friends_.ignored.push_back(i.substr(i.rfind('.') == std::string::npos ? 0 : i.rfind('.') + 1));
    friends_.chat_connected = conn.chat_server_connected();

    // The guild invitation lapses after the original dialog's 10 s; testing accepts.
    if (!g.inviter.empty()) {
        if (guild_invite_seen_ == Clock::time_point{}) guild_invite_seen_ = now;
        if (auto_accept_ && now - guild_invite_seen_ > std::chrono::seconds(2)) conn.answer_guild_invite(true);
        else if (now - guild_invite_seen_ > std::chrono::seconds(10)) conn.answer_guild_invite(false);
    } else {
        guild_invite_seen_ = {};
    }
    if (!r.inviter.empty() && auto_accept_) {
        if (raid_invite_seen_ == Clock::time_point{}) raid_invite_seen_ = now;
        else if (now - raid_invite_seen_ > std::chrono::seconds(2)) {
            conn.answer_raid_invite(true);
            raid_invite_seen_ = {};
        }
    }

    ui::SocialActions a;
    // Each off while its Lua window (ui/<pack>) runs.
    if (!ui::lua::replaces("GuildManagementWnd")) ui::guild_window(guild_, a);
    if (!ui::lua::replaces("ConfirmationDialogBox")) ui::guild_invite_prompt(guild_, a); // the Lua dialog asks it
    if (!ui::lua::replaces("RaidWindow")) ui::raid_window(raid_, a);
    if (!ui::lua::replaces("FriendsWindow")) ui::friends_window(friends_, a);

    // The buttons' command lines go as they are: no ${...} (mq::LiteralLine; app/lua_ui_social.cpp says why).
    mq::LiteralLine literal;
    if (a.guild_invite) conn.send_command("/guildinvite");
    if (a.guild_remove) conn.send_command("/guildremove " + *a.guild_remove);
    if (a.guild_leave) conn.send_command("/guildremove " + (conn.profile() ? conn.profile()->name : std::string()));
    if (a.guild_motd) conn.send_command("/guildmotd " + *a.guild_motd);
    if (a.guild_answer) conn.answer_guild_invite(*a.guild_answer);
    if (a.raid_invite) conn.send_command("/raidinvite");
    if (a.raid_answer) conn.answer_raid_invite(*a.raid_answer);
    if (a.raid_leave) conn.raid_leave();
    // /friends and /ignore toggle a name on or off the list.
    if (a.add_friend) conn.send_command("/friends " + *a.add_friend);
    if (a.remove_friend) conn.send_command("/friends " + *a.remove_friend);
    if (a.add_ignore) conn.send_command("/ignore " + *a.add_ignore);
    if (a.remove_ignore) conn.send_command("/ignore " + *a.remove_ignore);
}

} // namespace hailstone::app
