/* A test plugin for the server extension interface (sdk/hailstone_plugin.h, ABI 2; tests/native_extension_test.cpp):
 * it sets class masks, pets, switches and vitals from InitializePlugin and answers a few hooks. /exton and /extoff
 * switch it active and inactive; /extclass <id> [name [short]] names a class (set_class_name; no name: NULL).
 *
 * Built three times: ExtendTest (ABI 2); with HAILSTONE_TEST_ABI1, ExtendTestV1, a plugin as an ABI 1 client built
 * it (no hook table, the old host table, only the old export names), which must still load; and with
 * HAILSTONE_TEST_NEW_HOOKS, ExtendTestNew, the same as ExtendTest with its hook table exported under the new name.
 *
 * It keeps the SDK's names up to 0.6 (nmsc_plugin.h, NmscHost, NMSC_*, the `nmsc` global) on purpose, so the build
 * checks that a plugin written against them still compiles. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nmsc_plugin.h"

#ifdef HAILSTONE_TEST_ABI1
const NmscHost* nmsc = 0;
NMSC_EXPORT uint32_t nmsc_plugin_abi(void) { return 1; }
NMSC_EXPORT void nmsc_plugin_set_host(const NmscHost* h) { nmsc = h; }
NMSC_EXPORT void InitializePlugin(void) { WriteChatf("[ExtendTestV1] loaded"); }
#else
NMSC_PLUGIN

static void ext_on(void* user, const char* args) {
    (void)user;
    (void)args;
    nmsc->set_active(nmsc->host, 1);
}

static void ext_off(void* user, const char* args) {
    (void)user;
    (void)args;
    nmsc->set_active(nmsc->host, 0);
}

static void ext_class(void* user, const char* args) {
    unsigned id = 0;
    char name[64] = "", short_name[16] = "";
    int n;
    (void)user;
    if (!NMSC_HOST_HAS(nmsc, set_class_name)) return;
    n = sscanf(args ? args : "", "%u %63s %15s", &id, name, short_name);
    nmsc->set_class_name(nmsc->host, id, n >= 2 ? name : NULL, n >= 3 ? short_name : NULL);
}

static int is_spellcaster(int* out) {
    *out = 1;
    return 1;
}

static uint32_t guild_member_classes(uint32_t value) { return value >= 1000 ? value - 1000 : 0; }

static const char* label(int id, const char* text) {
    (void)text;
    return id == 42 ? "plugin label" : NULL;
}

static int hide_xtarget_slot(int slot) { return slot == 19; }

static int skill_gate(int32_t skill, uint32_t mask, int32_t profile_skill, int* has) {
    (void)mask;
    (void)profile_skill;
    if (skill != 53) return 0;
    *has = 0;
    return 1;
}

static int who_lines(const NmscWho* who, NmscLineFn add, void* ctx) {
    char line[128];
    snprintf(line, sizeof line, "who: %u", (unsigned)who->count);
    add(ctx, line);
    if (who->count) {
        const NmscWhoEntry* e = NMSC_WHO_ENTRY(who, 0);
        snprintf(line, sizeof line, "%s level %u", e->name, (unsigned)e->level);
        add(ctx, line);
    }
    return 1;
}

static const char* item_level_line(uint32_t spell_id) { return spell_id == 7 ? "Level Needed: test" : NULL; }

static int item_right_click(const NmscItemSlot* slot) {
    if (slot->slot != NMSC_SLOT_CURSOR) return 0;
    WriteChatf("[click] cursor");
    return 1;
}

static const char* list_level(const NmscItem* item) { return item->id == 5001 ? "L42" : NULL; }

/* The maps: the group member (labelled with its con colour) and the rat (green, layer 1); nothing else. */
static int map_spawn(const NmscSpawn* s, NmscMapSpawn* out) {
    if (s->in_group) {
        snprintf(out->label, sizeof out->label, "%s con %u", s->name, (unsigned)s->con_color);
        return 1;
    }
    if (s->id != 10) return 0;
    out->color = 0x00aa00;
    out->layer = 1;
    return 1;
}

/* /extnext <id>: next_spawn after that id. */
static void ext_next(void* user, const char* args) {
    NmscSpawn s;
    (void)user;
    s.size = sizeof s;
    if (nmsc->next_spawn(nmsc->host, (uint32_t)strtoul(args ? args : "0", NULL, 10), &s))
        WriteChatf("[next] %u", (unsigned)s.id);
    else
        WriteChatf("[next] none");
}

/* /extread: the readers, printed. */
static void ext_read(void* user, const char* args) {
    NmscPlayer p;
    NmscSpawn s;
    NmscSpell sp;
    NmscItem it;
    NmscBuff b;
    NmscItemSlot bag = {0, 24, 0, -1};
    uint32_t id = 0;
    int count = 0;
    char buf[64];
    struct {
        uint32_t size;
        uint32_t spawn_id;
    } old;
    (void)user;
    (void)args;
    p.size = sizeof p;
    if (nmsc->get_player(nmsc->host, &p))
        WriteChatf("[read] player %s %u class %u target %u group %d hp %lld/%lld", p.name, (unsigned)p.level,
                   (unsigned)p.class_id, (unsigned)p.target_id, (int)p.in_group, (long long)p.cur_hp, (long long)p.max_hp);
    s.size = sizeof s;
    while (nmsc->next_spawn(nmsc->host, id, &s)) {
        id = s.id;
        ++count;
    }
    WriteChatf("[read] spawns %d", count);
    if (nmsc->get_spawn(nmsc->host, 10, &s)) WriteChatf("[read] spawn %s npc %d", s.name, (int)s.is_npc);
    sp.size = sizeof sp;
    if (nmsc->get_spell(nmsc->host, 11, &sp)) WriteChatf("[read] spell %s cleric %u", sp.name, (unsigned)sp.class_level[1]);
    it.size = sizeof it;
    if (nmsc->get_item(nmsc->host, &bag, &it)) WriteChatf("[read] item %s x%u", it.name, (unsigned)it.stack_count);
    b.size = sizeof b;
    if (nmsc->get_buff(nmsc->host, 0, &b)) WriteChatf("[read] buff %u", (unsigned)b.spell_id);
    WriteChatf("[read] eqstr %d", nmsc->eqstr(nmsc->host, 999999, NULL, 0, buf, sizeof buf));
    /* A struct as an older plugin knew it: only its own fields are written. */
    old.size = sizeof old;
    old.spawn_id = 0;
    nmsc->get_player(nmsc->host, (NmscPlayer*)(void*)&old);
    WriteChatf("[read] old %u size %u", (unsigned)old.spawn_id, (unsigned)old.size);
}

/* /extmore: the rest of the host (folders, zone names, actions, the camera), printed. */
static void ext_more(void* user, const char* args) {
    char buf[512];
    uint32_t augs[6] = {1, 2, 3, 4, 5, 6};
    NmscPlayer p;
    NmscServer sv;
    (void)user;
    (void)args;
    nmsc->write_chat_color(nmsc->host, "[more] yellow", 15);
    WriteChatf("[more] server %d %s", nmsc->server_config_dir(nmsc->host, buf, sizeof buf), buf);
    buf[0] = 0;
    WriteChatf("[more] character %d %s", nmsc->character_config_dir(nmsc->host, buf, sizeof buf), buf);
    buf[0] = 0;
    nmsc->zone_name(nmsc->host, 1, buf, sizeof buf);
    WriteChatf("[more] zone %s %d", buf, nmsc->zone_name(nmsc->host, 999999, buf, sizeof buf));
    nmsc->set_window_pet(nmsc->host, 301);
    nmsc->clear_spell_recovery(nmsc->host);
    nmsc->close_spell_book(nmsc->host);
    WriteChatf("[more] inspect %d", nmsc->inspect_item(nmsc->host, 5001, augs));
    nmsc->set_max_camera_distance(nmsc->host, 90.0f);
    WriteChatf("[more] camera %d", (int)nmsc->max_camera_distance(nmsc->host));
    p.size = sizeof p;
    if (nmsc->get_player(nmsc->host, &p))
        WriteChatf("[more] player pet %u stand %u casting %u", (unsigned)p.pet_id, (unsigned)p.stand_state,
                   (unsigned)p.casting_spell_id);
    nmsc->set_hot_button(nmsc->host, 1, 2, 3, "HA1,A-1,0000000000000000,0,Attack");
    nmsc->set_hot_bar_page(nmsc->host, 1, 4);
    buf[0] = 0;
    WriteChatf("[more] hot %d %s page %d bad %d", nmsc->hot_button(nmsc->host, 1, 2, 3, buf, sizeof buf), buf,
               (int)nmsc->hot_bar_page(nmsc->host, 1), nmsc->hot_button(nmsc->host, 10, 0, 0, buf, sizeof buf));
    WriteChatf("[more] skill %d aa %u none %u", (int)nmsc->skill(nmsc->host, 53),
               (unsigned)nmsc->aa_value(nmsc->host, 777), (unsigned)nmsc->aa_value(nmsc->host, 778));
    sv.size = sizeof sv;
    if (nmsc->server_info(nmsc->host, &sv))
        WriteChatf("[more] server_info %s|%s|%u|%s|%s", sv.name, sv.host, (unsigned)sv.port, sv.world, sv.world_host);
    else
        WriteChatf("[more] server_info none");
}

/* For its Lua windows: info(text, {key = ...}) gives {echo, field, level, rows = {1, 2, 3}}; an option "mode". */
static int mode = 2;
static int get_mode(void* user) {
    (void)user;
    return mode;
}
static void set_mode(void* user, int value) {
    (void)user;
    mode = value;
}

static void out_int(NmscOut* out, int64_t i) {
    NmscValue v;
    memset(&v, 0, sizeof v);
    v.kind = NMSC_INT;
    v.i = i;
    nmsc->out_value(nmsc->host, out, &v);
}

static int ui_info(void* user, const NmscArgs* args, NmscOut* out) {
    NmscValue v, k;
    NmscPlayer p;
    int i;
    (void)user;
    nmsc->out_begin_table(nmsc->host, out);
    if (nmsc->arg(nmsc->host, args, 0, &v) && v.kind == NMSC_STRING) {
        nmsc->out_key(nmsc->host, out, "echo");
        nmsc->out_value(nmsc->host, out, &v);
    }
    if (nmsc->arg_field(nmsc->host, args, 1, "key", &k) && k.kind == NMSC_STRING) {
        nmsc->out_key(nmsc->host, out, "field");
        nmsc->out_value(nmsc->host, out, &k);
    }
    p.size = sizeof p;
    if (nmsc->get_player(nmsc->host, &p)) {
        nmsc->out_key(nmsc->host, out, "level");
        out_int(out, p.level);
    }
    nmsc->out_key(nmsc->host, out, "rows");
    nmsc->out_begin_array(nmsc->host, out);
    for (i = 1; i <= 3; ++i) out_int(out, i);
    nmsc->out_end(nmsc->host, out);
    nmsc->out_end(nmsc->host, out);
    return 1;
}

#ifdef HAILSTONE_TEST_NEW_HOOKS
NMSC_EXPORT const NmscHooks* hailstone_plugin_hooks(void) {
#else
NMSC_EXPORT const NmscHooks* nmsc_plugin_hooks(void) {
#endif
    static NmscHooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.size = sizeof hooks;
    hooks.is_spellcaster = is_spellcaster;
    hooks.guild_member_classes = guild_member_classes;
    hooks.label = label;
    hooks.hide_xtarget_slot = hide_xtarget_slot;
    hooks.skill_gate = skill_gate;
    hooks.who_lines = who_lines;
    hooks.item_level_line = item_level_line;
    hooks.item_right_click = item_right_click;
    hooks.list_level = list_level;
    hooks.map_spawn = map_spawn;
    return &hooks;
}

NMSC_EXPORT void InitializePlugin(void) {
    NmscPet pets[2] = {{300, 13}, {301, 13}};
    NmscVitals v;
    memset(&v, 0, sizeof v);
    v.has = NMSC_VITAL_MAX_HP;
    v.max_hp = 1234;
    nmsc->set_flag(nmsc->host, NMSC_FLAG_WIDE_ITEM_LINKS | NMSC_FLAG_HIDE_ANY_CLASS, 1);
    nmsc->set_player_classes(nmsc->host, 0x3);
    nmsc->set_character_classes(nmsc->host, "Gennro", 0x8001);
    nmsc->set_pet_list(nmsc->host, pets, 2);
    nmsc->set_vitals(nmsc->host, &v);
    AddCommand("/exton", ext_on, 0);
    AddCommand("/extoff", ext_off, 0);
    AddCommand("/extread", ext_read, 0);
    AddCommand("/extmore", ext_more, 0);
    AddCommand("/extclass", ext_class, 0);
    AddCommand("/extnext", ext_next, 0);
    nmsc->add_ui_function(nmsc->host, "info", ui_info, 0);
    nmsc->add_option(nmsc->host, "mode", "Mode", 0, 3, get_mode, set_mode, 0);
    nmsc->add_menu(nmsc->host, "Ext Window", "/exton");
}

NMSC_EXPORT void ShutdownPlugin(void) {
    RemoveCommand("/exton");
    RemoveCommand("/extoff");
    RemoveCommand("/extread");
    RemoveCommand("/extmore");
    RemoveCommand("/extclass");
    RemoveCommand("/extnext");
}
#endif
