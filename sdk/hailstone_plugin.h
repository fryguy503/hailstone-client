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
 * A call from a thread the plugin started does nothing and returns 0 / NULL (ability_state: -1), and a line in the
 * log says so, once. A plugin that works on a thread of its own hands its results over and acts on them in OnPulse.
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
#include "hailstone_follow.h"
#include "hailstone_world_geometry.h"

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

/*These are used by the h_get_spawns_data_bulk method*/
/*needs to be packed so that there is no padding*/
enum HS_BulkSpawnFlags {
    HS_BULK_SPAWN_NONE = 0,
    HS_BULK_SPAWN_AFK = 1 << 0,
    HS_BULK_SPAWN_AGGRESSIVE = 1 << 1,
    HS_BULK_SPAWN_ANON = 1 << 2,
    HS_BULK_SPAWN_BUYER = 1 << 3,
    HS_BULK_SPAWN_DEAD = 1 << 4,
    HS_BULK_SPAWN_DUCKING = 1 << 5,
    HS_BULK_SPAWN_FEIGNING = 1 << 6,
    HS_BULK_SPAWN_GM = 1 << 7,
    HS_BULK_SPAWN_INVIS = 1 << 8,
    HS_BULK_SPAWN_SUMMONED = 1 << 9,
    HS_BULK_SPAWN_LEVITATION = 1 << 10,
    HS_BULK_SPAWN_LINKDEAD = 1 << 11,
    HS_BULK_SPAWN_MOUNT = 1 << 12,
    HS_BULK_SPAWN_MOVING = 1 << 13,
    HS_BULK_SPAWN_NAMED = 1 << 14,
    HS_BULK_SPAWN_ROLEPLAYING = 1 << 15,
    HS_BULK_SPAWN_SITTING = 1 << 16,
    HS_BULK_SPAWN_SNEAKING = 1 << 17,
    HS_BULK_SPAWN_STANDING = 1 << 18,
    HS_BULK_SPAWN_STUNNED = 1 << 19,
    HS_BULK_SPAWN_TARGETABLE = 1 << 20,
    HS_BULK_SPAWN_TRADER = 1 << 21,
    HS_BULK_SPAWN_UNDERWATER = 1 << 22,
};
#pragma pack(push, 1)
typedef struct HS_Bulk_SpawnHeader {
    uint32_t spawnCount;
    float playerX;
    float playerY;
    float playerZ;
} HS_BulkSpawnHeader;
typedef struct HS_Bulk_SpawnFixedRecord {
    int32_t id;
    uint32_t flags;
    int32_t bodyTypeID;
    int32_t classID;
    int32_t conColorID;
    int64_t currentHps;
    int32_t genderId;
    int64_t guildID;
    float heading;
    float height;
    int32_t level;
    int32_t master;
    float maxRange;
    float maxRangeTo;
    int32_t pctHPs;
    int32_t playerState;
    int32_t raceID;
    float x;
    float y;
    float z;
    int32_t deity;
    /* Immediately followed on the wire by 7 length - prefixed strings :*/
    /* 1. BodyDesc, 2. CleanName, 3. DisplayName, 4. Name, 5. RaceName, 6. Suffix, 7. TypeDesc*/
} HS_BulkSpawnFixedRecord;
/* get_spawns_data_bulk: the header's size, and each record's fixed part before its seven strings. */
#define HS_BULK_SPAWN_HEADER_SIZE 16
#define HS_BULK_SPAWN_RECORD_SIZE 92
typedef struct HS_Bulk_BuffData {
    int32_t ID;
    int32_t casterID;
    int32_t duration;
    int32_t hitcount;
    int32_t spelltype;
    int32_t counterType;
    int32_t counterTotal;
    uint8_t slotType; /* HAILSTONE_BUFF_SLOT_BUFF / _SONG / _DISC*/
} HS_BulkBuffData;
/* get_buff_name_data_bulk: HS_Bulk_BuffData with the caster's name in place of its spawn id. One record per buff slot,
 * HAILSTONE_BUFF_NAME_RECORD_SIZE fixed bytes each, immediately followed by casterNameLength bytes of the caster's
 * name (no NUL); the next record starts after them. */
typedef struct HS_Bulk_BuffNameFixedRecord {
    int32_t ID;               /* spell id; 0 for an empty slot, whose other fields are all 0 */
    int32_t casterNameLength; /* bytes of the caster's name that follow; 0 for none */
    int32_t duration;         /* tics left, as get_buff_data_bulk's */
    int32_t hitcount;
    int32_t spelltype;
    int32_t counterType;
    int32_t counterTotal;
    uint8_t slotType; /* HAILSTONE_BUFF_SLOT_BUFF / _SONG / _DISC*/
} HS_Bulk_BuffNameFixedRecord;
typedef struct HS_Bulk_PetBuffData {
    int32_t id;
    int32_t duration;
    int32_t spellType;
} HS_BulkPetBuffData;
/* The header of a list-type bulk read (get_target_buff_data_bulk, get_avail_aa_data_bulk, get_avail_disc_data_bulk),
 * HS_BULK_LIST_HEADER_SIZE bytes before the first record. Like HS_Bulk_RaidHeader's count and total, it states both
 * how many records this buffer holds and how many the client has: count < total means out_size was too small to hold
 * them all, so retry with a larger buffer. Below HS_BULK_LIST_HEADER_SIZE bytes nothing is written (0 returned); with
 * nothing to list, the header alone goes out, count 0 and total 0. */
typedef struct HS_Bulk_ListHeader {
    int32_t count; /* records that follow in this buffer (as many whole records as fit) */
    int32_t total; /* records the client holds, written or not */
} HS_BulkListHeader;
/* get_target_buff_data_bulk: an HS_Bulk_ListHeader for the current target's buffs, then one record per buff,
 * HAILSTONE_TARGET_BUFF_RECORD_SIZE fixed bytes each, immediately followed by casterNameLength bytes of the caster's
 * name (no NUL); the next record starts after them. As many whole records as fit follow the header, in order, none
 * after the first that does not fit. */
typedef struct HS_Bulk_TargetBuffFixedRecord {
    int32_t spellID;          /* the buff's spell; entries with no spell are not sent */
    int32_t duration;         /* tics left (the server's count less the 6 s ticks since); negative: permanent */
    int32_t spellType;        /* 0 detrimental, 1 beneficial, 2 beneficial group-only (0 for an unknown spell) */
    int32_t slot;             /* the target's buff slot, as the server sent it (EQEmu: the mob's own slot index) */
    int32_t casterNameLength; /* bytes of the caster's name that follow; 0 for none */
} HS_BulkTargetBuffFixedRecord;
/* get_avail_aa_data_bulk and get_avail_disc_data_bulk: an HS_Bulk_ListHeader, then `count` records, each an
 * HAILSTONE_ID_NAME_RECORD (HAILSTONE_ID_NAME_RECORD_SIZE bytes) immediately followed by nameSize bytes of the name (no
 * NUL); the next record starts after them. As many whole records as fit follow the header, in order. */
typedef struct HAILSTONE_ID_NAME_RECORD {
    int32_t id;       /* an AA's ability (group) id, or a discipline's spell id */
    int32_t nameSize; /* bytes of the AA's or the discipline's name that follow; 0 when the client has none */
} HAILSTONE_ID_NAME_RECORD;
/* get_ability_data_bulk: an HS_Bulk_ListHeader, then `count` records, each an HS_Bulk_AbilityRecord
 * (HAILSTONE_ABILITY_RECORD_SIZE bytes) immediately followed by nameSize bytes of the name (no NUL); the next record
 * starts after them, in the Ability Book's order: the owned activated AAs, the listed disciplines, then the usable
 * skills (the combat page's, then the abilities page's). Passive entries are not listed. Match an entry by (kind, id),
 * not by its position. */
typedef struct HS_Bulk_AbilityRecord {
    int32_t kind;        /* HS_ABILITY_* */
    int32_t id;          /* a skill's id, a discipline's spell id, or an AA's ability (group) id */
    int32_t ready;       /* 1 its reuse has run out, 0 on reuse */
    int64_t remainingMs; /* until it is ready, 0 when ready */
    int64_t reuseMs;     /* its reuse time */
    int32_t nameSize;    /* bytes of the name that follow, no NUL */
} HS_BulkAbilityRecord;
#pragma pack(pop)

/* An extended target slot's type: HailstoneXTarget.type and HS_BulkXTargetData.targetType. The values are the
 * protocol's (EQEmu's XTargetType, zone/client.h). Named with HS_BULK_ (Hailstone's bulk reads) in front, as the
 * other bulk-read names are, so a plugin ported from MacroQuest, whose eqlib has the same names without it, can
 * include both. */
enum HS_BULK_XTargetTypes {
    HS_BULK_XTARGET_EMPTY_TARGET = 0,
    HS_BULK_XTARGET_AUTO_HATER = 1,
    HS_BULK_XTARGET_SPECIFIC_PC = 2,
    HS_BULK_XTARGET_SPECIFIC_NPC = 3,
    HS_BULK_XTARGET_TARGETS_TARGET = 4,
    HS_BULK_XTARGET_GROUP_TANK = 5,
    HS_BULK_XTARGET_GROUP_TANKS_TARGET = 6,
    HS_BULK_XTARGET_GROUP_ASSIST = 7,
    HS_BULK_XTARGET_GROUP_ASSIST_TARGET = 8,
    HS_BULK_XTARGET_GROUP_PULLER = 9,
    HS_BULK_XTARGET_GROUP_PULLER_TARGET = 10,
    HS_BULK_XTARGET_GROUP_MARK1 = 11,
    HS_BULK_XTARGET_GROUP_MARK2 = 12,
    HS_BULK_XTARGET_GROUP_MARK3 = 13,
    HS_BULK_XTARGET_RAID_ASSIST1 = 14,
    HS_BULK_XTARGET_RAID_ASSIST2 = 15,
    HS_BULK_XTARGET_RAID_ASSIST3 = 16,
    HS_BULK_XTARGET_RAID_ASSIST1_TARGET = 17,
    HS_BULK_XTARGET_RAID_ASSIST2_TARGET = 18,
    HS_BULK_XTARGET_RAID_ASSIST3_TARGET = 19,
    HS_BULK_XTARGET_RAID_MARK1 = 20,
    HS_BULK_XTARGET_RAID_MARK2 = 21,
    HS_BULK_XTARGET_RAID_MARK3 = 22,
    HS_BULK_XTARGET_MY_PET = 23,
    HS_BULK_XTARGET_MY_PET_TARGET = 24,
    HS_BULK_XTARGET_MY_MERCENARY = 25,
    HS_BULK_XTARGET_MY_MERCENARY_TARGET = 26,
    HS_BULK_XTARGET_TYPE_COUNT = 27
};
/* get_xtarget_data_bulk: one record per extended target slot, in slot order (HS_BULK_XTARGET_RECORD_SIZE bytes). */
#pragma pack(push, 1)
typedef struct HS_Bulk_XTargetData {
    int32_t targetType; /* the slot's type (HS_BULK_XTARGET_*), as get_xtarget's HailstoneXTarget.type */
    int32_t id;         /* the slot's spawn id, 0 for none */
    int32_t aggroPct;   /* the aggro meter's percent for this slot (0 without the meter or past its 20 slots) */
    int32_t pctHPs;     /* the spawn's HP percent; 0 without a spawn or for a corpse */
} HS_BulkXTargetData;
#pragma pack(pop)

/* get_raid_data_bulk: the raid roster, its header and one record per member
 * (HS_BULK_RAID_HEADER_SIZE and HS_BULK_RAID_RECORD_SIZE bytes today). The header is always written when out_size
 * holds it, with count 0 and active 0 outside a raid; only whole records follow it.
 *
 * A reader steps from the first record at header.size by header.recordSize bytes, for header.count records. The
 * record may grow at its end in a later client: read recordSize, never a fixed stride, and read only the fields the
 * record's size covers. version changes only when an older reader can no longer read the header or a record at all.
 * total is the roster's member count, so a caller whose buffer cut the roster short can see it from count < total
 * and retry with a larger one (the rule every list-type bulk read follows, HS_Bulk_ListHeader).
 *
 * A member not in this zone has spawnID 0 and no HP, position or pet (the percentages are -1, the rest 0); the
 * roster's level and class still answer. */
#define HS_BULK_RAID_HEADER_SIZE 40
#define HS_BULK_RAID_RECORD_SIZE 124
#define HS_BULK_RAID_NO_INDEX 0xffffffffu
#pragma pack(push, 1)
typedef struct HS_Bulk_RaidHeader {
    uint32_t size;        /* sizeof(HS_Bulk_RaidHeader); the first record starts here */
    uint32_t version;     /* 1; changes only when an older reader cannot read the data */
    uint32_t count;       /* member records that follow (what fit in out_size) */
    uint32_t recordSize;  /* bytes between records; may grow, so step by it */
    uint32_t leaderIndex; /* 0-based member, HS_BULK_RAID_NO_INDEX for none */
    uint32_t targetIndex; /* the raid window's selected member, same encoding */
    int32_t active;       /* in a raid */
    int32_t locked;
    int32_t lootType;     /* 1 Leader Only, 2 Leader and Group Leader, 3 Leader Selected, 4 Entire Raid */
    uint32_t total;       /* the roster's member count, not only those written; count < total: buffer too small */
} HS_BulkRaidHeader;

typedef struct HS_Bulk_RaidData {
    int32_t slot;         /* 0-based roster order */
    uint32_t spawnID;     /* 0: not in this zone */
    int32_t group;        /* 0..11; -1 ungrouped */
    uint32_t flags;       /* HS_BULK_RAID_* */
    int32_t level;        /* 0 when unknown */
    int32_t classID;      /* 0 when unknown */
    int32_t pctHPs;       /* -1 unknown */
    int32_t pctMana;      /* -1 unknown; group members only */
    int32_t pctEndurance; /* -1 unknown; group members only */
    uint32_t petSpawnID;  /* 0 none; never a mercenary or a temporary pet */
    int32_t petPctHPs;    /* -1 unknown or no pet */
    float x, y, z;        /* the zone's coordinates, as HailstoneSpawn.x/y/z */
    float distance;       /* 3D from the player; 0 when the member is not here */
    char name[64];
} HS_BulkRaidData;
#pragma pack(pop)
#define HS_BULK_RAID_LEADER 0x0001u
#define HS_BULK_RAID_GROUP_LEADER 0x0002u
#define HS_BULK_RAID_MAIN_ASSIST 0x0004u
#define HS_BULK_RAID_MARKER 0x0008u /* the member holds the raid Marker role */
#define HS_BULK_RAID_WAYPOINTER 0x0010u
#define HS_BULK_RAID_LOOTER 0x0020u
/* bit 0x0040 is reserved: this client keeps no offline flag for a raid member */
#define HS_BULK_RAID_PRESENT 0x0080u
#define HS_BULK_RAID_ME 0x0100u
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

enum HailstoneBinaryKind {
    HAILSTONE_BINARY_EXECUTABLE = 1,
    HAILSTONE_BINARY_CALLING_PLUGIN = 2
};
typedef struct HailstoneBinaryMeasurement {
    uint32_t size;
    uint32_t kind;
    uint64_t byte_count;
    uint64_t fnv1a63;       /* FNV-1a64 of all file bytes, with the sign bit cleared */
    uint8_t sha256[32];
} HailstoneBinaryMeasurement;

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
    uint32_t zone_instance;
    uint32_t observation_flags; /* HAILSTONE_PLAYER_* flags */
} HailstonePlayer;
#define HAILSTONE_PLAYER_PROMPT_SAFE 16u

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
    /* -- appended in ABI 3; check size before reading -- */
    uint64_t incarnation;      /* nonzero full-spawn identity; never reuse metadata on a different token */
    uint32_t observation_flags; /* HAILSTONE_SPAWN_*_KNOWN; absence means unknown, not zero */
    uint32_t mana_percent, endurance_percent;
    uint32_t social_flags;     /* HAILSTONE_SOCIAL_* */
    uint32_t anon;             /* 0 ordinary, 1 anonymous, 2 roleplaying */
    char last_name[64];
    char guild_name[64];       /* resolved from the received guild-name cache; empty when unknown */
    char display_name[64];     /* client's ordinary display name, before title/last-name/leadership decorations */
} HailstoneSpawn;

#define HAILSTONE_SPAWN_HP_KNOWN 1u
#define HAILSTONE_SPAWN_MANA_KNOWN 2u
#define HAILSTONE_SPAWN_ENDURANCE_KNOWN 4u
#define HAILSTONE_SOCIAL_TRADER 1u
#define HAILSTONE_SOCIAL_BUYER 2u
#define HAILSTONE_SOCIAL_LINKDEAD 4u
#define HAILSTONE_SOCIAL_LFG 8u
#define HAILSTONE_SOCIAL_AFK 16u
#define HAILSTONE_RELATION_FRIEND 1u
#define HAILSTONE_RELATION_GUILD 2u

#define HAILSTONE_MAP_MARKER 0u
#define HAILSTONE_MAP_FIND 1u
#define HAILSTONE_MERCHANT_CURSOR_OCCUPIED 1u
#define HAILSTONE_MERCHANT_SELL_REFUSED 2u
#define HAILSTONE_MERCHANT_KEPT 4u
typedef struct HailstoneMerchantSellContext {
    uint32_t size, merchant_id, inventory_slot, item_id, quantity;
    HailstoneItemSlot item_slot; /* lossless carried slot; scalar inventory_slot is its top-level slot */
    uint32_t flags; /* blocked sale reasons, HAILSTONE_MERCHANT_*; consume without sending when nonzero */
} HailstoneMerchantSellContext;

/* How a spawn shows on the game's maps (HailstoneHooks.map_spawn). */
typedef struct HailstoneMapSpawn {
    uint32_t size;
    uint32_t color; /* 0xRRGGBB */
    int32_t layer;  /* the map's layer 0..3: its layer switches hide it */
    char label[64]; /* shown as the map shows its labels; "" the dot alone */
} HailstoneMapSpawn;

/* Optional map presentation ownership. No provider/zero flags keeps stock behavior.
 * Restrictive flags combine across active providers. The first CUSTOM_SPAWNS
 * provider exclusively supplies extension spawn marks; map_visible still denies. */
#define HAILSTONE_MAP_POLICY_CUSTOM_SPAWNS 1u
#define HAILSTONE_MAP_POLICY_HIDE_STOCK_NPCS 2u
#define HAILSTONE_MAP_POLICY_DISABLE_NPC_FIND 4u
typedef struct HailstoneMapPolicy {
    uint32_t size;
    uint32_t flags;
    char name[64]; /* optional plain display name, NUL terminated */
    /* Optional tails: the original 72-byte prefix remains valid. Providers advance revision when their
     * policy content changes; readers receive a host-effective revision including provider lifetimes.
     * context is host-issued connection/zone/player lifetime, ignored in provider answers. */
    uint64_t revision;
    uint64_t context;
} HailstoneMapPolicy;

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
    char serial[17]; /* received instance identity, empty when unavailable */
    uint32_t instance_id;
    uint32_t augment_ids[6]; /* zero for known empty sockets; bags never report their contents as augments */
    uint32_t configuration_complete; /* zero when installed configuration cannot be represented */
} HailstoneItem;

typedef struct HailstoneBuff {
    uint32_t size;
    uint32_t spell_id; /* 0 an empty slot */
    uint32_t tics; /* as the server last sent it; a permanent buff's negative count as its bits (-1 is 0xffffffff) */
    uint32_t level, caster_id;
} HailstoneBuff;

typedef struct HailstoneXTarget {
    uint32_t size;
    uint32_t type;   /* the slot's type on this side (HS_BULK_XTARGET_*): 0 empty, 1 auto hater, ... */
    uint32_t status; /* the server's: 0 empty, 1 a spawn, 2 a named absent one */
    uint32_t spawn_id;
    char name[64];
} HailstoneXTarget;

/* ---- the plugin's Lua windows (ABI 2, HAILSTONE_CAP_UI; docs/plugins.md "Plugin windows") ----
 * A plugin's windows are Lua files in its folder's ui/<pack>/ (plugins/<name>/ui/modern/, ui/gamepad/ ...), loaded
 * with the player's UI pack while the plugin is loaded. They reach the plugin through hs.UI.plugins.Get("<name>"): its
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
    HailstoneFollowProfileFn follow_profile;
    HailstoneMountProfileFn mount_profile;
    /* UI + EXTEND. -1 abstains, 0 denies, 1 allows. Deny wins across extensions. */
    int (*map_visible)(const HailstoneSpawn* spawn, uint32_t purpose);
    /* PACKETS + EXTEND. A validated carried-item right click while a merchant is open.
     * Nonzero consumes the click. The extension implements its own wire protocol. */
    int (*merchant_right_click)(const HailstoneMerchantSellContext* context);
    const char* (*item_flags)(const HailstoneItem* item, const char* stock_flags);
    const char* (*item_description_append)(const HailstoneItem* item);
    uint64_t (*item_presentation_revision)(void);
    const char* (*loot_confirmation)(const HailstoneItem* root, const uint32_t* descendants, size_t count, int complete);
    void (*world_frame)(const HailstoneWorldFrame* frame);
    void (*world_frame_result)(const HailstoneWorldReceipt* receipt);
    const char* (*zone_display_name)(uint32_t zone_id, uint32_t instance_id);
    /* UI + EXTEND. Nonzero supplies a validated map policy; zero abstains. */
    int (*map_policy)(HailstoneMapPolicy* out);
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
    /* A function its windows call as hs.UI.plugins.Get("<plugin>").<name>(...) (or .call("<name>", ...)). */
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

    /* ---- added later in ABI 3: append only, in order, and check HAILSTONE_HOST_HAS(field) before calling (an older
     * client's table ends before; new entries go at the end, never between these: a plugin built with an older header
     * finds each by its position) ----
     * The bank window opens as a right click on a banker opens it (CBankWnd::Activate), here without that click's
     * class and range checks: `banker` a spawn id, 0 for the player itself (as the NMS patch's vault Bank button);
     * the window closes when that spawn is out of reach. 1 open (or already open), 0 not (HAILSTONE_CAP_ACTIONS). */
    int (*open_bank)(void* host, uint32_t banker);
    int32_t (*get_buff_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int32_t (*get_pet_buff_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int32_t (*get_spawns_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    /* How the item on the cursor now got there, HAILSTONE_CURSOR_ORIGIN_* (HAILSTONE_CAP_READ; without it, or with
     * nothing on the cursor, UNKNOWN). The answer belongs to that one item: any change to the cursor the client does
     * not recognise reads UNKNOWN, never an older item's origin. Read it beside get_item(cursor), then act with
     * /destroycursor <id> or /stowcursor <id>, which do nothing unless the cursor still holds that id. */
    uint32_t (*cursor_origin)(void* host);
    int32_t (*get_xtarget_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int32_t (*get_target_buff_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int32_t (*get_avail_aa_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int32_t (*get_avail_disc_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int32_t (*get_buff_name_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    /* The raid roster: HS_Bulk_RaidHeader, then up to HS_Bulk_RaidData records, as many whole records as fit;
     * bytes written, or 0 when out_size cannot hold the header. Outside a raid the header says active 0, count 0.
     * Added later in ABI 3: test HAILSTONE_HOST_HAS(host, get_raid_data_bulk) first (HAILSTONE_CAP_READ). */
    int32_t (*get_raid_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    /* Uses one of the player's abilities as its Ability Book row or hot button does (HAILSTONE_CAP_ACTIONS): 1 used,
     * 0 not (the player has none such, it is passive, or the client refused it for the moment). `kind` is
     * HS_ABILITY_*; `name` is what the Ability Book takes: a skill's name or number, a discipline's name with or
     * without its rank, an AA's name or group id. Added later in ABI 3: test HAILSTONE_HOST_HAS(host, use_ability)
     * first. */
    int (*use_ability)(void* host, int kind, const char* name);
    /* Whether that ability is ready (HAILSTONE_CAP_READ): 1 ready, 0 on reuse, -1 the player has none such, the client
     * is not in a zone, the plugin lacks `read`, or it was called off the client's thread. When it is found,
     * *remaining_ms and *reuse_ms are filled for both (either may be NULL). `kind` and `name` as use_ability's. Added
     * later in ABI 3: test HAILSTONE_HOST_HAS(host, ability_state) first. */
    int (*ability_state)(void* host, int kind, const char* name, int64_t* remaining_ms, int64_t* reuse_ms);
    /* The Ability Book's entries in one call (HAILSTONE_CAP_READ): an HS_Bulk_ListHeader (count, total), then up to
     * `count` HS_Bulk_AbilityRecord records, each followed by its name, as many whole records as fit. Every usable
     * entry the book lists: the owned activated AAs, the disciplines and the skills, in its order. An entry's id is a
     * skill's id, a discipline's spell id or an AA's ability (group) id; readiness is as ability_state's. Before the
     * profile, or with no connection, count and total are 0; without `read`, 0 bytes. Below the header's size nothing
     * is written; count < total means out_size was too small. Added later in ABI 3: test
     * HAILSTONE_HOST_HAS(host, get_ability_data_bulk) first. */
    int32_t (*get_ability_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    /* Targets a raid member in the zone, or a member's pet (HAILSTONE_CAP_READ and HAILSTONE_CAP_TARGET): spawn_id
     * as get_raid_data_bulk's spawnID, incarnation as get_spawn's HailstoneSpawn.incarnation for it, so a spawn id
     * reused since is refused. 1 targeted, 0 not. Added later in ABI 3: test HAILSTONE_HOST_HAS first. */
    int (*select_raid_member)(void* host, uint32_t spawn_id, uint64_t incarnation);
    /* READ: local decoder, at most 256 KiB. READ + UI: local item inspector. */
    int (*decode_item_packet)(void* host, const void* data, size_t size, HailstoneItemSlot* slot, HailstoneItem* item);
    int (*inspect_item_packet)(void* host, const void* data, size_t size);
    /* MOVE + EXTEND: literal start/stop/toggle, no command evaluation. */
    int (*manual_follow)(void* host, const char* action);
    int (*get_follow_status)(void* host, HailstoneFollowStatus* out);
    uint32_t (*get_memorized_spell)(void* host, uint32_t index);
    /* Asks the server to change the raid (HAILSTONE_CAP_READ and HAILSTONE_CAP_ACTIONS), as the raid window's
     * leader does: HAILSTONE_RAID_MOVE puts the member `name` in `group`, as get_raid_data_bulk numbers groups (0..11,
     * -1 ungrouped). Only the raid's leader, in a locked raid, to a group with room; 1 sent, 0 refused. The server
     * decides; the roster changes when it says so. Added later in ABI 3: test HAILSTONE_HOST_HAS first. */
    int (*raid_action)(void* host, const char* name, uint32_t action, int32_t group);
    uint64_t (*monotonic_ms)(void* host);
    uint32_t (*world_capabilities)(void* host);
    /* Highest upward drawn opaque/masked surface at or below seed_z (the caller's
     * bounded ceiling): terrain, static/passable props, current animated scenery,
     * doors and placed objects. Excludes invisible collision, undersides,
     * transparent surfaces, creatures, dropped items, ghosts and missiles.
     * Masked triangles use their footprint; the CPU does not sample texture alpha. */
    int (*world_floor_below)(void* host, uint64_t frame, uint32_t epoch, float x, float y, float seed_z, float* out_z);
    int (*world_anchor)(void* host, uint64_t frame, uint32_t epoch, uint32_t spawn_id, HailstoneWorldAnchor* out);
    int (*world_submit)(void* host, const HailstoneWorldSubmission* submission);
    /* UI + EXTEND, current frame only. PLANAR certifies complete drawn-surface
     * coverage, including current props/animation, rather than sampled flatness.
     * MISMATCH/MISSING supplies an interior witness or its rounded float value
     * with validated source face and positive-area interior region. Budget exhaustion
     * or incomplete scene proof returns DEFERRED and leaves out unchanged. */
    int (*world_surface_patch)(void* host, const HailstoneWorldSurfacePatch* query,
                               HailstoneWorldSurfacePatchResult* out);
    /* READ: measures the running executable file or this caller's admitted loaded library copy.
     * No arbitrary paths or hardware identifiers. File measurement is not remote memory attestation.
     * Unreadable/changed files, a changed plugin-copy digest or unknown kind fail without changing out. */
    int (*measure_binary)(void* host, uint32_t kind, HailstoneBinaryMeasurement* out);
    /* READ. Current aggregate map policy. Initialize out->size; zero flags is stock. */
    int (*get_map_policy)(void* host, HailstoneMapPolicy* out);
    /* EXTEND: the server's name for a player class (1-16), shown wherever the client names the class (the
     * inventory, group and raid windows, /who, character select, ${Me.Class}, ...), and its short name ("VAN",
     * shown where the client abbreviates); a guildmaster of the class (20-35) reads "<name> Guildmaster". NULL or ""
     * shows the stock name. Display only: class ids, class checks and packets stay the stock ones. At most 32 bytes
     * for the name and 4 for the short name, no control characters: a longer name or another class id is refused
     * (nothing changes; the first refusal writes a line in the log). The names are the plugin's: they go when it
     * unloads, when the server's rules take `extend` away, and when the session leaves the server. When two plugins
     * or scripts name one class the latest call wins; a call with both empty takes this plugin's back. Added later
     * in ABI 3: test HAILSTONE_HOST_HAS(host, set_class_name) first. */
    void (*set_class_name)(void* host, uint32_t class_id, const char* name, const char* short_name);
} PluginHostApi;
/* What use_ability and ability_state take: the kinds of ability the Ability Book lists. The values are stable. */
enum HS_AbilityKinds {
    HS_ABILITY_SKILL = 0,      /* a skill (Bash, Kick, ...) */
    HS_ABILITY_DISCIPLINE = 1, /* a combat ability (a discipline) */
    HS_ABILITY_ALT = 2         /* an activated alternate ability */
};
/* Not known: it was on the cursor at zone-in, replaced a cursor item, or came by a path the client does not mark.
 * Treat it as the player's; never destroy it unattended. */
#define HAILSTONE_CURSOR_ORIGIN_UNKNOWN 0
/* The player picked it up from their own inventory (a click, a stack split). */
#define HAILSTONE_CURSOR_ORIGIN_INVENTORY 1
/* The server put it on the empty cursor: summoned items, tradeskill results and quest rewards (EQEmu's cursor queue),
 * and also a ground item the player picked up and an item looted onto the cursor (EQEmu sends those to the empty
 * cursor as item packets too). So SERVER does not mean the player did not choose it. */
#define HAILSTONE_CURSOR_ORIGIN_SERVER 2
#define HAILSTONE_BUFF_RECORD_SIZE 29
#define HAILSTONE_BUFF_NAME_RECORD_SIZE 29 /* get_buff_name_data_bulk's fixed part, before the name */
/* get_buff_data_bulk's last byte: the window the slot belongs to (0 also for an empty slot). */
/* The records run long buffs, then songs, then the one discipline slot (RoF2: 0-41, 42-61, 62); the counts are the
 * client's, so read this byte rather than a record's position. */
#define HAILSTONE_BUFF_SLOT_BUFF 0 /* a long buff slot */
#define HAILSTONE_BUFF_SLOT_SONG 1 /* a song slot; was "1 for a song", so old checks still hold */
#define HAILSTONE_BUFF_SLOT_DISC 2 /* the discipline slot, the last, sent only with room for one record more */
#define HAILSTONE_PET_BUFF_RECORD_SIZE 12
#define HS_BULK_XTARGET_RECORD_SIZE 16
#define HAILSTONE_TARGET_BUFF_RECORD_SIZE 20
#define HAILSTONE_ID_NAME_RECORD_SIZE 8
#define HAILSTONE_ABILITY_RECORD_SIZE 32 /* get_ability_data_bulk's fixed part, before the name */
#define HS_BULK_LIST_HEADER_SIZE 8 /* HS_Bulk_ListHeader */
/* raid_action's actions. */
#define HAILSTONE_RAID_MOVE 1u /* move a member to another group, or out of every group */
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
