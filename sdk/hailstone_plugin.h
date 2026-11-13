/* SPDX-License-Identifier: MIT (sdk/LICENSE) */
/* Hailstone compiled plugins: the C interface a plugin (a .dll / .so / .dylib) is built against.
 *
 * The names follow MacroQuest's plugin callbacks, so an MQ plugin's structure carries over: export the callbacks
 * you need (InitializePlugin, ShutdownPlugin, OnPulse, SetGameState, OnZoned, OnIncomingChat, ...) and talk to the
 * client through the host table (PluginHostApi) it hands you before InitializePlugin (HAILSTONE_PLUGIN in one source
 * file keeps it in the global `host` and gives WriteChatf, DoCommand, AddCommand, ParseMacroData,
 * AddTopLevelObject, ...). MQ's sources do not build as they are: they read eqgame.exe's memory layouts, which
 * this client does not have; the game is reached through TLOs instead (ParseMacroData("${Me.PctHPs}") or the TLO
 * functions).
 *
 * The server decides what loads (docs/plugins.md): its own plugins (sent from its client_files/plugins/) always
 * load; the player's only when its hailstone.cfg allows them, with the capabilities it grants (HAILSTONE_CAP_*). A
 * server without a hailstone.cfg sets no rules. The client loads a copy of exactly the bytes it hashed.
 * Calls the plugin may not make fail (return 0 / -1) and say so in the chat window.
 *
 * Server extensions (ABI 2): a plugin can also change the client's rules where a server's own client differs from
 * the stock one, without a change to Hailstone. It exports a hook table (hailstone_plugin_hooks, HailstoneHooks below)
 * the core asks at those points, and sets what it knows through the host table (class masks, the pet list, rule
 * switches, vitals). Several plugins may do so at once: the client asks them in load order and takes the first
 * answer (docs/plugins.md). A plugin is active from its load; set_active(0) keeps it out until it knows its server.
 * All of this needs HAILSTONE_CAP_EXTEND.
 *
 * Web pages (ABI 3): a plugin opens its own tabs of the in-game browser, draws them in its windows and talks to
 * the page (web_*; HAILSTONE_CAP_WEB; docs/browser.md).
 *
 * Compatibility: the tables only grow at their end and carry their size. A plugin built for an older ABI loads in a
 * newer client; one built for a newer ABI does not load in an older client.
 *
 * Plain C, no C++ types across the boundary: build with any compiler. Every string the client passes is valid only
 * during the call; strings the plugin returns are copied by the client before the call returns.
 *
 * Threads: the client calls the plugin on its own thread, and the host table is for that thread only, from inside
 * one of the plugin's callbacks (InitializePlugin, OnPulse, a command, a hook ...): the client's state has no locks.
 * A call from a thread the plugin started does nothing and returns 0 / NULL (a line in the log says so, once). A
 * plugin that works on a thread of its own hands its results over and acts on them in OnPulse.
 *
 * Exceptions: a C++ plugin must not let an exception out of any function the client calls (callbacks, commands,
 * TLOs, hooks, UI functions). The client catches what it can and treats the call as unanswered (an exception out of
 * InitializePlugin or hailstone_plugin_hooks unloads the plugin with everything it registered), but an exception
 * crossing a C interface is undefined behaviour, and one from a plugin built with another compiler or runtime may
 * end the client. */
#ifndef HAILSTONE_PLUGIN_H
#define HAILSTONE_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HAILSTONE_PLUGIN_ABI 3

#if defined(_WIN32)
#define HAILSTONE_EXPORT __declspec(dllexport)
#else
#define HAILSTONE_EXPORT __attribute__((visibility("default")))
#endif
/* An exported callback, with C names in a C++ plugin too: HAILSTONE_API void OnPulse(void) { ... } */
#ifdef __cplusplus
#define HAILSTONE_EXTERN_C extern "C"
#define HAILSTONE_HOST_GLOBAL extern "C" { const PluginHostApi* host = 0; }
#else
#define HAILSTONE_EXTERN_C
#define HAILSTONE_HOST_GLOBAL const PluginHostApi* host = 0;
#endif
#define HAILSTONE_API HAILSTONE_EXTERN_C HAILSTONE_EXPORT

/* Capabilities (plugin/policy.h) */
#define HAILSTONE_CAP_READ 0x001u
#define HAILSTONE_CAP_COMMANDS 0x002u
#define HAILSTONE_CAP_TARGET 0x004u
#define HAILSTONE_CAP_ACTIONS 0x008u
#define HAILSTONE_CAP_MOVE 0x010u
#define HAILSTONE_CAP_CHAT 0x020u
#define HAILSTONE_CAP_EVENTS 0x040u
#define HAILSTONE_CAP_UI 0x080u
#define HAILSTONE_CAP_PACKETS 0x100u
#define HAILSTONE_CAP_EXTEND 0x200u /* change the client's rules as its server's client does: hooks, set_flag, ... */
#define HAILSTONE_CAP_LOGIN 0x400u  /* load before the zone-in: at the world login and character select */
#define HAILSTONE_CAP_NET 0x800u    /* open the plugin's own TCP connections (tcp_open ...) */
#define HAILSTONE_CAP_WEB 0x1000u   /* web pages: /url, /www, the web_* tabs (ABI 3); http(s), within the server's [web] allow */

/* MQ's game states (SetGameState) */
#define GAMESTATE_PRECHARSELECT (-1)
#define GAMESTATE_CHARSELECT 1
#define GAMESTATE_CHARCREATE 2
#define GAMESTATE_INGAME 5
#define GAMESTATE_LOGGINGIN 253
#define GAMESTATE_UNLOADING 255

/* A TLO value the plugin gives back. */
enum {
    HAILSTONE_NIL = 0,
    HAILSTONE_BOOL = 1,
    HAILSTONE_INT = 2,
    HAILSTONE_FLOAT = 3,
    HAILSTONE_STRING = 4,
    HAILSTONE_TABLE = 5 /* (arguments) */
};
typedef struct HailstoneValue {
    int kind;
    int64_t i;     /* HAILSTONE_BOOL (0 / 1), HAILSTONE_INT */
    double f;      /* HAILSTONE_FLOAT */
    const char* s; /* HAILSTONE_STRING (copied when the function returns) */
} HailstoneValue;

/* A TLO the plugin adds (AddTopLevelObject): asked for ${Name[index]} (member NULL: the TLO's own value) and for
 * ${Name[index].Member[member_index]}. Return nonzero when `out` was set; zero is NULL. */
typedef int (*HailstoneTloFn)(void* user, const char* index, const char* member, const char* member_index,
                              HailstoneValue* out);
typedef void (*HailstoneCommandFn)(void* user, const char* args);

/* A packet in OnPacket: change data / size in place (resize_packet for a new size). */
typedef struct HailstonePacket {
    uint16_t opcode;
    uint8_t* data;
    size_t size;
} HailstonePacket;
enum { HAILSTONE_STREAM_WORLD = 0, HAILSTONE_STREAM_ZONE = 1 };
enum { HAILSTONE_IN = 0, HAILSTONE_OUT = 1 };

/* ---- server extensions (ABI 2) ---- */

/* Rule switches (set_flag): on while the plugin is active. */
#define HAILSTONE_FLAG_WIDE_ITEM_LINKS 0x01u /* item links with 8-digit ids (items above 0xFFFFF) */
#define HAILSTONE_FLAG_ITEMS_ANY_CLASS 0x02u /* CanUseItem / CanEquipItem true for everything: only the server checks */
#define HAILSTONE_FLAG_HIDE_ANY_CLASS 0x04u  /* moving breaks Hide unless sneaking, for every class (the Rogue rule) */
#define HAILSTONE_FLAG_AA_ANY_LEVEL 0x08u    /* alternate abilities from level 1 */
#define HAILSTONE_FLAG_AA_TRAIN_ALL 0x10u    /* the AA window's Train All box buys every rank (OP_AAAction 4) */
/* Class checks over the player's classes (the class mask set_player_classes gives, else the profile's class): */
#define HAILSTONE_FLAG_AUGMENT_CLASSES 0x20u  /* an augment for a worn item must share one of them, and the item takes them */
#define HAILSTONE_FLAG_RESEARCH_CLASSES 0x40u /* the Wizard / Magician / Necromancer / Enchanter research containers */
#define HAILSTONE_FLAG_FOCUS_CLASSES 0x80u    /* focus: SPA 411 and an AA's classes test any of them, and Bards and pure
                                          * melee classes are no longer left without spell and AA focus */
#define HAILSTONE_FLAG_BACKSTAB_EITHER_HAND 0x100u /* Backstab takes a piercer in the secondary hand too, not only the
                                               * primary (an older client ignores the bit) */
#define HAILSTONE_FLAG_SOLO_ADVENTURE 0x200u /* an adventure recruiter takes a request from a player in no group or raid
                                         * (an older client ignores the bit) */

/* One of the player's pets (set_pet_list). */
typedef struct HailstonePet {
    uint32_t spawn_id;
    uint32_t class_id; /* the summoning class; 0 unknown or charmed */
} HailstonePet;
/* The local character's HP, mana and endurance as the server's client answers them (set_vitals): a field counts
 * when its HAILSTONE_VITAL_* bit is in `has`, and the client's formula is used for the others. */
#define HAILSTONE_VITAL_MAX_HP 0x01u
#define HAILSTONE_VITAL_CUR_HP 0x02u
#define HAILSTONE_VITAL_MAX_MANA 0x04u
#define HAILSTONE_VITAL_CUR_MANA 0x08u
#define HAILSTONE_VITAL_MAX_ENDURANCE 0x10u
#define HAILSTONE_VITAL_CUR_ENDURANCE 0x20u
typedef struct HailstoneVitals {
    uint32_t has;
    int32_t max_hp, cur_hp, max_mana, cur_mana, max_endurance, cur_endurance;
} HailstoneVitals;

/* A /who reply (OP_WhoAllResponse) for HailstoneHooks.who_lines. Read entries with HAILSTONE_WHO_ENTRY: entry_size may
 * be larger than the plugin's HailstoneWhoEntry in a later client. Strings may be empty, never NULL. */
typedef struct HailstoneWhoEntry {
    const char* name;
    const char* guild;
    const char* zone_name; /* with flags 0x200 */
    const char* account;
    uint32_t flags; /* 1 AFK, 8 linkdead, 0x10 trader, 0x20 buyer, 0x40 RIP, 0x200 zone name, 0x400 offline */
    uint32_t class_id, level, race, zone_id;
    uint32_t format, rank_string, zone_string; /* eqstr ids, 0xffffffff none */
} HailstoneWhoEntry;
typedef struct HailstoneWho {
    uint32_t size;       /* sizeof(HailstoneWho) of the client */
    uint32_t entry_size; /* sizeof(HailstoneWhoEntry) of the client */
    uint32_t count;
    const HailstoneWhoEntry* entries;
    uint32_t title_string; /* eqstr id, 0xffffffff none */
    const char* line;
    uint32_t footer_string, footer_count;
    uint32_t footer_values[3];
} HailstoneWho;
#define HAILSTONE_WHO_ENTRY(who, i) \
    ((const HailstoneWhoEntry*)((const unsigned char*)(who)->entries + (size_t)(i) * (who)->entry_size))
/* Adds one line of output (who_lines). */
typedef void (*HailstoneLineFn)(void* ctx, const char* line);

/* ---- the game state, read through the host (ABI 2, HAILSTONE_CAP_READ) ----
 * Each struct starts with `size`: set it to sizeof the struct before the call. The client fills what both it and the
 * plugin know (a later client may know more fields) and returns nonzero when there was something to read. Names are
 * copied into the struct, cut to fit and NUL-terminated. */

/* An inventory slot (the client's ItemSlot): type 0 the player's possessions; `slot` the RoF2 slot number;
 * `sub` the index in the bag at `slot`, -1 the slot itself; `aug` an augment, -1 none. */
typedef struct HailstoneItemSlot {
    int16_t type, slot, sub, aug;
} HailstoneItemSlot;
#define HAILSTONE_SLOT_CURSOR 33

/* The server this login is on (server_info). */
typedef struct HailstoneServer {
    uint32_t size;
    char name[128];       /* the saved server's name, as the player or the server list named it ("" without one) */
    char host[128];       /* its login server */
    uint32_t port;
    char world[128];      /* the world's name, as the login server listed it */
    char world_host[128];
} HailstoneServer;

typedef struct HailstonePlayer {
    uint32_t size;
    uint32_t spawn_id; /* 0 before the player is in a zone */
    char name[64];
    uint32_t level, race;
    uint32_t class_id;      /* the profile's */
    uint32_t visible_class; /* what the client's checks see (a plugin's cast_class) */
    uint32_t class_mask;    /* the player's classes: the multiclass mask, else the profile's class */
    int64_t cur_hp, max_hp; /* Cur_HP / Max_HP: the items' HP included, which EQEmu's own HP update leaves out */
    uint32_t cur_mana, max_mana, cur_endurance, max_endurance;
    uint32_t target_id;
    uint32_t zone_id;
    int32_t game_state; /* GAMESTATE_*; 0 before the first */
    int32_t in_group, in_raid, spell_book_open;
    uint32_t buff_slots;    /* get_buff's indexes are 0 .. buff_slots - 1 */
    uint32_t xtarget_slots; /* get_xtarget's likewise */
    uint32_t casting_spell_id;  /* the spell being cast, 0 none */
    uint32_t scribing_spell_id; /* the spell being scribed into the book, 0 none */
    uint32_t stand_state;       /* 100 standing, 110 sitting, 105 feigning, 115 ducking, 120 dead */
    uint32_t pet_id;            /* the pet the pet window shows, 0 none */
    int32_t filter_pc_spells;   /* the Options window's "PC Spells" chat filter setting, as the client keeps it */
    int32_t in_zone;            /* 1 while in a zone, camping included: not while zoning or at character select */
} HailstonePlayer;

typedef struct HailstoneSpawn {
    uint32_t size;
    uint32_t id;
    char name[64];
    uint32_t level, class_id, race;
    uint32_t owner_id; /* a pet's owner, 0 none */
    uint32_t guild_id; /* 0xffffffff none */
    int32_t is_npc, is_corpse;
    float x, y, z;  /* the zone's coordinates (MQ's Y, X, Z order is the plugin's business) */
    float heading;  /* 0..512 */
    uint32_t hp_percent;
    /* -- added later in ABI 2 (check size) -- */
    uint32_t rider_id;      /* a mount's rider, 0 none */
    int32_t is_mercenary;
    int32_t is_gm;
    int32_t in_group;       /* one of the player's group (by name, as the group window lists it) */
    uint32_t con_color;     /* the consider colour as a chat colour (2 green, 18 light blue, 4 blue, 10 white, 15 yellow,
                             * 13 red, 6 grey), 0 none (the player, corpses) */
    uint32_t body_types[4]; /* the spawn record's properties ([0] is the body type) */
    uint32_t body_type_count;
} HailstoneSpawn;

/* How a spawn shows on the game's maps (HailstoneHooks.map_spawn). */
typedef struct HailstoneMapSpawn {
    uint32_t size;
    uint32_t color; /* 0xRRGGBB */
    int32_t layer;  /* the map's layer 0..3: its layer switches hide it */
    char label[64]; /* shown as the map shows its labels; "" the dot alone */
} HailstoneMapSpawn;

typedef struct HailstoneSpell {
    uint32_t size;
    uint32_t id;
    char name[64];
    uint8_t class_level[16]; /* class 1..16; 255 cannot use */
    int32_t timer_id;        /* the combat ability timer (the file's; ability_timer_id may change it) */
    uint32_t cast_time_ms, recovery_time_ms, recast_time_ms;
    int32_t mana;
    uint32_t target_type, skill;
    int32_t is_skill;
    uint32_t gem_icon;
} HailstoneSpell;

typedef struct HailstoneItem {
    uint32_t size;
    uint32_t id;
    char name[64];
    uint32_t item_type, item_class; /* EQEmu's ItemType (20 scroll, ...); 0 common, 1 container, 2 book */
    uint32_t classes, races;        /* bit masks */
    uint32_t icon, stack_count, charges;
    uint32_t click_spell, scroll_spell; /* the effects' spell ids, 0 none */
} HailstoneItem;

typedef struct HailstoneBuff {
    uint32_t size;
    uint32_t spell_id; /* 0 an empty slot */
    uint32_t tics, level, caster_id;
} HailstoneBuff;

typedef struct HailstoneXTarget {
    uint32_t size;
    uint32_t type;   /* the slot's type on this side: 0 empty target, 1 auto hater, 3 a specific NPC, ... */
    uint32_t status; /* the server's: 0 empty, 1 a spawn, 2 a named absent one */
    uint32_t spawn_id;
    char name[64];
} HailstoneXTarget;

/* ---- the plugin's Lua windows (ABI 2, HAILSTONE_CAP_UI; docs/plugins.md "Plugin windows") ----
 * A plugin's windows are Lua files in its folder's ui/<pack>/ (plugins/<name>/ui/modern/, ui/gamepad/ ...), loaded
 * with the player's UI pack while the plugin is loaded. They reach the plugin through eq.Plugin("<name>"): its
 * functions take Lua values and give one back. A function reads its arguments with arg / arg_field and builds its
 * result with the out_* functions: a value, or an array or table opened with out_begin_*, filled (in a table:
 * out_key before each value; a whole-number key, "12", is the Lua integer 12) and closed with out_end. Returning 0
 * gives nil. */
/* A web tab's state (web_state). Like the structs above it only grows at its end and the client fills what both
 * know, but its `size` is a size_t (as ABI 3 shipped it): set it to sizeof(HailstoneWebState). */
typedef struct HailstoneWebState {
    size_t size;
    char url[1024];
    char title[256];
    int loading, can_back, can_forward;
    int closed; /* by the browser or the rules; the tab takes no more calls */
} HailstoneWebState;

typedef struct HailstoneArgs HailstoneArgs;
typedef struct HailstoneOut HailstoneOut;
typedef int (*HailstoneUiFn)(void* user, const HailstoneArgs* args, HailstoneOut* out);
typedef int (*HailstoneOptionGetFn)(void* user);
typedef void (*HailstoneOptionSetFn)(void* user, int value);

/* What the core asks a plugin (all optional; NULL = the stock answer). "Answered" functions return nonzero when
 * they set *out, zero for the stock answer. They run only while the plugin is active, and need HAILSTONE_CAP_EXTEND.
 * The names in brackets are the original client's functions the hook stands in for. */
typedef struct HailstoneHooks {
    uint32_t size; /* sizeof(HailstoneHooks) the plugin was built with */

    /* -- the connection -- */
    /* Each frame in the zone: nonzero runs the zone-in's last step again (OnZoned follows). */
    int (*zone_entry_retry)(void);
    /* One of the game's own commands by its handler name ("loot", "alt", ...) before the client runs it;
     * nonzero = handled. */
    int (*game_command)(const char* handler, const char* args);
    /* A /who reply: add the lines to print in place of the stock ones and return nonzero. */
    int (*who_lines)(const HailstoneWho* who, HailstoneLineFn add, void* ctx);

    /* -- classes and spells -- */
    /* The class the client's checks see in place of the profile's while it returns nonzero (a cast spoof). */
    uint8_t (*cast_class)(void);
    /* [EQ_Character::IsSpellcaster] for the local character. */
    int (*is_spellcaster)(int* out);
    /* [EQ_Spell::GetSpellLevelNeeded] */
    int (*spell_level_needed)(uint32_t spell_id, uint32_t class_id, uint8_t* out);
    /* [CharacterZoneClient::BardCastBard]: nonzero = never a song (`bard_level` the spell record's Bard byte). */
    int (*song_blocked)(uint8_t bard_level);
    /* [CEverQuest::StartCasting]: nonzero = print "You begin casting %1." for the player's own cast. */
    int (*own_begin_cast_line)(uint32_t spell_id);
    /* [CharacterZoneClient::CanUseMemorizedSpellSlot] */
    int (*can_use_gem)(int gem, int* out);
    /* The spell's combat ability timer id (CARecastTimerID). */
    int (*ability_timer_id)(uint32_t spell_id, int32_t* out);
    /* [GetDisciplineReuseTimer]: the ready time in Unix seconds. */
    int (*discipline_reuse)(int32_t timer_id, uint32_t* out);
    /* Around [PcZoneClient::doCombatAbility]: before, nonzero answers without running it (*allowed 0: refused);
     * after, `used` is the client's own answer. */
    int (*before_combat_ability)(uint32_t spell_id, int* allowed);
    void (*after_combat_ability)(uint32_t spell_id, int used);
    /* The combat abilities window's effect label; NULL = the stock text. */
    const char* (*combat_effect_label)(void);
    /* A guild member's classes from the class value the server stores, as a class mask; 0 = a plain class id. */
    uint32_t (*guild_member_classes)(uint32_t class_value);
    /* The classes whose max HP / mana / endurance formulas the server takes the best of, as a class mask; 0 = the
     * player's class only. */
    uint32_t (*vitals_classes)(uint8_t class_id);

    /* -- skills -- (asked only with a class mask set) */
    /* [CharacterZoneClient::HasSkill] before the original: *has 0 no skill, 1 has it. */
    int (*skill_gate)(int32_t skill, uint32_t class_mask, int32_t profile_skill, int* has);
    /* The class the skill value is then read with; 0 = the profile's. */
    uint32_t (*skill_spoof_class)(int32_t skill, uint32_t class_mask);
    /* The tracking skill check. */
    int (*tracking_allowed)(uint8_t class_id, uint32_t class_mask, uint32_t tracking_skill, int* out);

    /* -- displays -- */
    /* The item display's "Level Needed" line for a spell scroll; NULL = the stock line. */
    const char* (*item_level_line)(uint32_t spell_id);
    /* [__GetLabelFromEQ]: the text for a label id; NULL keeps `text`. */
    const char* (*label)(int id, const char* text);
    /* The extended target window leaves the slot out (no row, not targeted, kept for the server). */
    int (*hide_xtarget_slot)(int slot);

    /* -- items -- */
    /* [CInvSlot::HandleRButtonUp] before the client's own: nonzero = taken. The plugin may run the client's own
     * later with the host's right_click_item. */
    int (*item_right_click)(const HailstoneItemSlot* slot);
    /* The merchant and bazaar lists' Lvl column for an item; NULL = the stock column. */
    const char* (*list_level)(const HailstoneItem* item);

    /* -- added later in ABI 2 (a plugin built before leaves them NULL) -- */
    /* [CharacterZoneClient::GetFocusReuseMod]: nonzero = no reuse focus for the spell's cool-down. */
    int (*reuse_focus_dropped)(uint32_t spell_id);
    /* Before [CharacterZoneClient::UseSkill]: nonzero answers without running it (*allowed 0: refused; the plugin
     * says why). */
    int (*before_use_skill)(uint32_t skill, int* allowed);
    /* A typed or scripted command line before the client runs it (after MacroQuest's ${} parsing): a line in its
     * place, or NULL to keep it. */
    const char* (*command_line)(const char* line);
    /* The game's maps (the map window, the minimap, the zone map), which on their own show only the group: nonzero
     * shows the spawn as *out says (filled with the defaults first). Asked for every spawn but the player's, each
     * time a map is drawn. */
    int (*map_spawn)(const HailstoneSpawn* spawn, HailstoneMapSpawn* out);
} HailstoneHooks;


/* The client's functions a plugin calls (the host table): one per plugin, handed over by hailstone_plugin_set_host. */
/* WARNING! ALWAYS APPEND: ORDER MATTERS IN PluginHostApi (see size below) */
/* WARNING!  ALWAYS APPEND: ORDER MATTERS IN PluginHostApi (see size below) */
/* WARNING!  ALWAYS APPEND: ORDER MATTERS IN PluginHostApi (see size below) */
/* WARNING!  ALWAYS APPEND: ORDER MATTERS IN PluginHostApi (see size below) */
typedef struct PluginHostApi {
    uint32_t abi;  /* HAILSTONE_PLUGIN_ABI */
    uint32_t size; /* sizeof(PluginHostApi) of the client: later ABIs only add fields at the end */
    void* host;    /* pass to every function */

    uint32_t (*caps)(void* host);              /* what the server lets this plugin do */
    const char* (*name)(void* host);           /* the plugin's name */
    void (*log)(void* host, int level, const char* text); /* the client's log: 0 debug .. 3 error */

    /* MQ's WriteChatColor: a line in the chat window (color 0xRRGGBB, 0 the default). */
    void (*write_chat)(void* host, const char* text, uint32_t color);
    /* MQ's DoCommand: a command line, as typed (${...} parsed), held to the plugin's capabilities. */
    void (*do_command)(void* host, const char* line);
    /* MQ's AddCommand / RemoveCommand ("/name"). */
    int (*add_command)(void* host, const char* name, HailstoneCommandFn fn, void* user);
    void (*remove_command)(void* host, const char* name);

    /* MQ's ParseMacroData: ${...} replaced; writes at most out_size bytes (NUL-terminated) and returns the full
     * length, or -1 without HAILSTONE_CAP_READ. */
    int (*parse)(void* host, const char* text, char* out, size_t out_size);
    /* MQ's AddTopLevelObject / RemoveTopLevelObject. add_tlo returns 0 when the name is already a TLO (the client's
     * or another plugin's); remove_tlo removes only a TLO this plugin added. */
    int (*add_tlo)(void* host, const char* name, HailstoneTloFn fn, void* user);
    void (*remove_tlo)(void* host, const char* name);

    /* A packet to the server (HAILSTONE_CAP_PACKETS); through the other plugins' outgoing hooks. */
    int (*send_packet)(void* host, int stream, uint16_t opcode, const void* data, size_t size);
    /* OnPacket's packet at a new size (the new bytes are zero); updates p->data and p->size. */
    int (*resize_packet)(void* host, HailstonePacket* p, size_t size);

    /* The directory for the plugin's own files (created on demand): writes `out` and returns the length. */
    int (*config_dir)(void* host, char* out, size_t out_size);
    /* The client's Dear ImGui context (ImGui::SetCurrentContext; the plugin must be built with the same Dear ImGui
     * version), or NULL without HAILSTONE_CAP_UI. */
    void* (*imgui_context)(void* host);

    /* ---- ABI 2: server extensions (HAILSTONE_CAP_EXTEND) ---- */
    /* Whether the core asks this plugin's hooks and uses its switches, vitals and so on: 1 (the default) or 0. */
    void (*set_active)(void* host, int active);
    /* An HAILSTONE_FLAG_* switch on (1) or off (0). */
    void (*set_flag)(void* host, uint32_t flag, int on);
    /* Multiclass: the player's classes (bit c-1 = class c; 0 = the character select mask, else the profile's
     * class), and a character's at character select by name (0 removes it); the client shows them ("War/Clr"). */
    void (*set_player_classes)(void* host, uint32_t mask);
    void (*set_character_classes)(void* host, const char* name, uint32_t mask);
    /* Several pets: the server's list of the player's pets (the focused pet stays while listed). */
    void (*set_pet_list)(void* host, const HailstonePet* pets, size_t count);
    /* The local character's vitals as the server's client answers them; NULL clears. The values given also become
     * the player's current points (as the server's client writes them into its profile) until the next update. */
    void (*set_vitals)(void* host, const HailstoneVitals* v);

    /* ---- ABI 2: reading the game state (HAILSTONE_CAP_READ; the structs above) ---- */
    int (*get_player)(void* host, HailstonePlayer* out);
    int (*get_spawn)(void* host, uint32_t spawn_id, HailstoneSpawn* out);
    /* The spawn with the lowest id above `after_id` (0 starts): walks every spawn in the zone. */
    int (*next_spawn)(void* host, uint32_t after_id, HailstoneSpawn* out);
    int (*get_spell)(void* host, uint32_t spell_id, HailstoneSpell* out);
    int (*get_item)(void* host, const HailstoneItemSlot* slot, HailstoneItem* out);
    int (*get_buff)(void* host, uint32_t index, HailstoneBuff* out);
    int (*get_xtarget)(void* host, uint32_t index, HailstoneXTarget* out);
    /* A game string (eqstr_us.txt) with %1.. filled from `args`; writes at most out_size bytes (NUL-terminated) and
     * returns the full length, -1 when there is no such string. */
    int (*eqstr)(void* host, uint32_t id, const char* const* args, size_t count, char* out, size_t out_size);
    /* The client's own right click on an inventory slot, as without the plugin's item_right_click
     * (HAILSTONE_CAP_ACTIONS). `class_id` nonzero: the click's casting check sees that class (a scribe spoof). */
    int (*right_click_item)(void* host, const HailstoneItemSlot* slot, uint32_t class_id);

    /* ---- ABI 2: the plugin's Lua windows (HAILSTONE_CAP_UI) ---- */
    /* A function its windows call as eq.Plugin("<plugin>").<name>(...) (or .call("<name>", ...)). */
    int (*add_ui_function)(void* host, const char* name, HailstoneUiFn fn, void* user);
    void (*remove_ui_function)(void* host, const char* name);
    /* The arguments: their count; one as a value (a table gives HAILSTONE_TABLE); a table argument's field by key.
     * Strings are valid during the call. Zero when there is no such argument or field. */
    size_t (*arg_count)(void* host, const HailstoneArgs* args);
    int (*arg)(void* host, const HailstoneArgs* args, size_t index, HailstoneValue* out);
    int (*arg_field)(void* host, const HailstoneArgs* args, size_t index, const char* key, HailstoneValue* out);
    /* The result. */
    void (*out_value)(void* host, HailstoneOut* out, const HailstoneValue* v);
    void (*out_begin_array)(void* host, HailstoneOut* out);
    void (*out_begin_table)(void* host, HailstoneOut* out);
    void (*out_key)(void* host, HailstoneOut* out, const char* key);
    void (*out_end)(void* host, HailstoneOut* out);
    /* An option in the Options window's plugin section: max 1 a switch, else a slider from min to max. The plugin
     * keeps the value (its own settings file): get reads it, set changes it. */
    int (*add_option)(void* host, const char* name, const char* label, int min, int max, HailstoneOptionGetFn get,
                      HailstoneOptionSetFn set, void* user);
    /* An entry in the packs' menus (the EQ menu, the gamepad pack's More page): choosing it runs `command`. */
    int (*add_menu)(void* host, const char* label, const char* command);

    /* ---- ABI 2: more of the client ---- */
    /* A chat line in one of the game's chat colours (the colour numbers the game's own messages use: 15 yellow,
     * 2 green, 6 grey ...; 0 the default), where write_chat takes an RGB value. */
    void (*write_chat_color)(void* host, const char* text, uint32_t color);
    /* The plugin's folders for the saved server (servers/<profile>/plugin-config/<name>/) and for the character
     * (servers/<profile>/characters/<world>/<character>/plugin-config/<name>/), created on demand: as config_dir,
     * 0 when there is none (before the world login; before a character is chosen). */
    int (*server_config_dir)(void* host, char* out, size_t out_size);
    int (*character_config_dir)(void* host, char* out, size_t out_size);
    /* A zone's short name ("qeynos2") by id: as eqstr, -1 for an unknown id (HAILSTONE_CAP_READ). */
    int (*zone_name)(void* host, uint32_t zone_id, char* out, size_t out_size);
    /* Actions (HAILSTONE_CAP_ACTIONS). The spell book closes; the global spell recovery ends now; the pet window shows
     * another of the player's pets (only this client's choice: the server is not told); an item's display opens as a
     * click on its link would (OP_ItemLinkClick; `augments` six ids or NULL). */
    void (*close_spell_book)(void* host);
    void (*clear_spell_recovery)(void* host);
    void (*set_window_pet)(void* host, uint32_t spawn_id);
    int (*inspect_item)(void* host, uint32_t item_id, const uint32_t* augments);
    /* The camera's zoom limit, and a new one (HAILSTONE_CAP_ACTIONS; the current zoom is cut to it). */
    float (*max_camera_distance)(void* host);
    void (*set_max_camera_distance)(void* host, float distance);
    /* The server this login is on (HAILSTONE_CAP_READ): 1 and `out` filled once the world login started, else 0. A
     * plugin for one server tells it from another server that sends the same packets by these (its name, its host). */
    int (*server_info)(void* host, HailstoneServer* out);
    /* The player's combat abilities (the profile's discipline list, 0..299): the spell id at `index`, 0 for none
     * (HAILSTONE_CAP_READ). */
    uint32_t (*combat_ability)(void* host, int32_t index);
    /* The hot buttons (10 bars of 10 pages of 12; all 0-based), each as the stock character ini's string
     * ("%c%d,%c%d,%s,%d,%s", SaveHotButtonsToIni; "" an empty button). hot_button gives its length, 0 empty, -1 out
     * of range or before a character's buttons are loaded (HAILSTONE_CAP_READ); set_hot_button puts one ("" or NULL
     * clears it) and is saved with the character, 1 done (HAILSTONE_CAP_ACTIONS). A bar's page: hot_bar_page (-1 out of
     * range), set_hot_bar_page. */
    int (*hot_button)(void* host, int32_t bar, int32_t page, int32_t button, char* out, size_t out_size);
    int (*set_hot_button)(void* host, int32_t bar, int32_t page, int32_t button, const char* spec);
    int32_t (*hot_bar_page)(void* host, int32_t bar);
    void (*set_hot_bar_page)(void* host, int32_t bar, int32_t page);
    /* The profile's value of a skill (0..99) as the server sent it, without item or spell bonuses; 0 untrained or
     * before the profile (HAILSTONE_CAP_READ). */
    int32_t (*skill)(void* host, uint32_t skill);
    /* An owned alternate ability by its ability (group) id, all ranks share it: the value its slot holds (the profile
     * sends the rank, OP_RespondAA the points spent on it); 0 when not owned or its rank is not in the AA table
     * (HAILSTONE_CAP_READ). */
    uint32_t (*aa_value)(void* host, int32_t group);

    /* ---- ABI 2: the plugin's own TCP connections (HAILSTONE_CAP_NET; docs/plugins.md "Connections") ----
     * For a service the server runs beside the game (a card game's, say): the client owns the sockets, so the plugin
     * needs no socket code of its own. Nothing waits for the network. tcp_open starts connecting: a handle above 0,
     * or 0 when it cannot (no permission, already HAILSTONE_TCP_MAX open). A name is looked up in the background while
     * the handle counts as connecting; one that does not resolve closes it (tcp_state -1, tcp_error says why).
     * tcp_state: 1 connected, 0 still connecting, -1 closed (refused, dropped, closed by the peer; tcp_error says why,
     * as eqstr returns). tcp_send takes every byte and returns the count, or -1 once closed: what
     * the system cannot take at once waits in the client and goes out at the next tcp_* call. tcp_recv gives what has
     * arrived: the count, 0 for nothing yet, -1 once closed with nothing left. tcp_close ends one; unloading the plugin
     * closes its others. */
    int32_t (*tcp_open)(void* host, const char* address, uint16_t port);
    int32_t (*tcp_state)(void* host, int32_t handle);
    int64_t (*tcp_send)(void* host, int32_t handle, const void* data, size_t size);
    int64_t (*tcp_recv)(void* host, int32_t handle, void* out, size_t out_size);
    void (*tcp_close)(void* host, int32_t handle);
    int (*tcp_error)(void* host, int32_t handle, char* out, size_t out_size);

    /* ---- ABI 3: web pages (HAILSTONE_CAP_WEB; docs/browser.md) ----
     * The plugin's own tabs of the in-game browser: http(s) pages on the hosts the server's [web] allow names. A tab
     * id is never 0; the plugin's tabs close when it unloads. */
    /* A new tab of width x height pixels; 0 when refused (the chat window says why). */
    uint32_t (*web_open)(void* host, const char* url, int width, int height);
    int (*web_navigate)(void* host, uint32_t tab, const char* url);
    /* "back", "forward", "reload", "stop" */
    int (*web_nav)(void* host, uint32_t tab, const char* action);
    void (*web_close)(void* host, uint32_t tab);
    /* Runs script in the page; calls the page's window.hailstone.onmessage(message); types text at its cursor. */
    int (*web_eval)(void* host, uint32_t tab, const char* script);
    int (*web_post)(void* host, uint32_t tab, const char* message);
    int (*web_text)(void* host, uint32_t tab, const char* text);
    /* The page's next window.hailstone.post(): its full length (cut to fit out, NUL-terminated), -1 for none. Given
     * a buffer it is taken off the queue, even when cut; with out NULL (or out_size 0) only its length comes back
     * and it stays queued, so call that first to make room for a long one (at most 1 MB). */
    int (*web_message)(void* host, uint32_t tab, char* out, size_t out_size);
    /* Set `out->size` first (the client fills what both know, as with the structs above); 0 for no such tab. */
    int (*web_state)(void* host, uint32_t tab, HailstoneWebState* out);
    /* From OnUpdateImGui, inside an ImGui window: draws the page width x height (the tab takes that size) and gives it
     * the mouse, and the keyboard once clicked. 1 while hovered. */
    int (*web_view)(void* host, uint32_t tab, float width, float height);

    /* ---- added later in ABI 3: check HAILSTONE_HOST_HAS(field) before calling (an older client's table ends
     * before) ----
     * The bank window opens as a right click on a banker opens it (CBankWnd::Activate), here without that click's
     * class and range checks: `banker` a spawn id, 0 for the player itself (as the NMS patch's vault Bank button);
     * the window closes when that spawn is out of reach. 1 open (or already open), 0 not (HAILSTONE_CAP_ACTIONS). */
    int (*open_bank)(void* host, uint32_t banker);
    int32_t (*get_buff_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int32_t (*get_pet_buff_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int32_t (*get_spawns_data_bulk)(void* host, unsigned char* out, int32_t out_size);
} PluginHostApi;
#define HAILSTONE_BUFF_RECORD_SIZE 29
#define HAILSTONE_PET_BUFF_RECORD_SIZE 12
/* Whether this client's host table has `field` (one added after the plugin's ABI) and it is set. */
#define HAILSTONE_HOST_HAS(h, field)                                                                                   \
    ((h) && (h)->size >= offsetof(PluginHostApi, field) + sizeof((h)->field) && (h)->field)
#define HAILSTONE_TCP_MAX 8

/* ---- what a plugin exports (all optional but the first two, which HAILSTONE_PLUGIN defines; declare each with
 * HAILSTONE_API so a C++ plugin exports it under its C name) ----
 *   uint32_t hailstone_plugin_abi(void);                     returns HAILSTONE_PLUGIN_ABI
 *   void hailstone_plugin_set_host(const PluginHostApi* h);  called first
 *   void InitializePlugin(void);  void ShutdownPlugin(void);
 *   void OnPulse(void);           void SetGameState(int state);
 *   void OnBeginZone(void);       void OnEndZone(void);    void OnZoned(void);
 *   void OnAddSpawn(uint32_t spawn_id);  void OnRemoveSpawn(uint32_t spawn_id);
 *   int  OnIncomingChat(const char* line, uint32_t color);   (HAILSTONE_CAP_EVENTS; the result is ignored, as in MQ)
 *   int  OnFilterChat(const char* line, uint32_t color);     (HAILSTONE_CAP_EVENTS; nonzero hides the line)
 *   void OnUpdateImGui(void);                                (HAILSTONE_CAP_UI)
 *   int  OnPacket(int stream, int direction, HailstonePacket* p);  (HAILSTONE_CAP_PACKETS; nonzero drops it)
 *   const HailstoneHooks* hailstone_plugin_hooks(void);
 *                                   (ABI 2, HAILSTONE_CAP_EXTEND; read once after InitializePlugin)
 *
 * Up to 0.6 the three hailstone_plugin_* exports were named nmsc_plugin_*. The client takes either name;
 * HAILSTONE_PLUGIN exports both, so the plugin also loads in those clients. A plugin with hooks that should work in
 * them exports nmsc_plugin_hooks too (returning the same table).
 */

#ifndef HAILSTONE_HOST /* the rest is for plugins; the client defines HAILSTONE_HOST */

extern const PluginHostApi* host;

/* In one source file of the plugin: the two required exports (under both names, see above) and the `host`
 * global. */
#define HAILSTONE_PLUGIN                                                                                               \
    HAILSTONE_HOST_GLOBAL                                                                                              \
    HAILSTONE_API uint32_t hailstone_plugin_abi(void) { return HAILSTONE_PLUGIN_ABI; }                                 \
    HAILSTONE_API void hailstone_plugin_set_host(const PluginHostApi* h) { host = h; }                                 \
    HAILSTONE_API uint32_t nmsc_plugin_abi(void) { return HAILSTONE_PLUGIN_ABI; }                                      \
    HAILSTONE_API void nmsc_plugin_set_host(const PluginHostApi* h) { host = h; }

/* MQ-style helpers over `host` (plain C). */
#include <stdarg.h>
#include <stdio.h>
static inline void WriteChatf(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (host) host->write_chat(host->host, buf, 0);
}
static inline void DoCommand(const char* line) {
    if (host) host->do_command(host->host, line);
}
static inline void DoCommandf(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    DoCommand(buf);
}
static inline int AddCommand(const char* name, HailstoneCommandFn fn, void* user) {
    return host ? host->add_command(host->host, name, fn, user) : 0;
}
static inline void RemoveCommand(const char* name) {
    if (host) host->remove_command(host->host, name);
}
static inline int ParseMacroData(const char* text, char* out, size_t out_size) {
    return host ? host->parse(host->host, text, out, out_size) : -1;
}
static inline int AddTopLevelObject(const char* name, HailstoneTloFn fn, void* user) {
    return host ? host->add_tlo(host->host, name, fn, user) : 0;
}
static inline void RemoveTopLevelObject(const char* name) {
    if (host) host->remove_tlo(host->host, name);
}

#endif /* HAILSTONE_HOST */

#ifdef __cplusplus
}
#endif

#endif
