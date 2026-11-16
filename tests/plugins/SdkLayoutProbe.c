/* SDK-only layout fixtures: canonical negotiation, rejected contracts, the released ABI 3 prefix, and the
 * explicitly selected earlier preview. Frozen legacy declarations deliberately do not use PluginHostApi. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hailstone_plugin.h"

#ifndef HAILSTONE_SDK_PROBE_MODE
#define HAILSTONE_SDK_PROBE_MODE 0
#endif

static unsigned* setter_calls;
static unsigned* initialize_calls;
HAILSTONE_API void sdk_probe_counters(unsigned* setters, unsigned* initializes) {
    setter_calls = setters;
    initialize_calls = initializes;
}
#if HAILSTONE_SDK_PROBE_MODE != 0
static void setter_called(void) { if (setter_calls) ++*setter_calls; }
#endif

/* Through get_xtarget_data_bulk: 81 function slots. Only these shared entry points are called by the fixture. */
typedef struct FrozenCommonApi {
    uint32_t abi, size;
    void* host;
    uint32_t (*caps)(void*);
    const char* (*name)(void*);
    void (*log)(void*, int, const char*);
    void (*write_chat)(void*, const char*, uint32_t);
    void (*do_command)(void*, const char*);
    int (*add_command)(void*, const char*, HailstoneCommandFn, void*);
    void (*remaining[75])(void);
} FrozenCommonApi;
typedef struct FrozenReleasedApi {
    FrozenCommonApi common;
    int32_t (*get_target_buff_data_bulk)(void*, unsigned char*, int32_t);
    int32_t (*get_avail_aa_data_bulk)(void*, unsigned char*, int32_t);
    int32_t (*get_avail_disc_data_bulk)(void*, unsigned char*, int32_t);
    int32_t (*get_buff_name_data_bulk)(void*, unsigned char*, int32_t);
} FrozenReleasedApi;
typedef struct FrozenReleasedRaidApi {
    FrozenReleasedApi released;
    int32_t (*get_raid_data_bulk)(void*, unsigned char*, int32_t);
} FrozenReleasedRaidApi;
typedef struct FrozenReleasedAbilityApi {
    FrozenReleasedRaidApi raid;
    int (*use_ability)(void*, int, const char*);
    int (*ability_state)(void*, int, const char*, int64_t*, int64_t*);
    int32_t (*get_ability_data_bulk)(void*, unsigned char*, int32_t);
} FrozenReleasedAbilityApi;
typedef struct FrozenPreviewApi {
    FrozenCommonApi common;
    int (*get_raid_roster)(void*, HailstoneRaidRoster*);
    int (*get_raid_member)(void*, uint64_t, uint32_t, HailstoneRaidMember*);
    int (*select_raid_member)(void*, uint64_t, uint32_t, uint64_t);
    int (*decode_item_packet)(void*, const void*, size_t, HailstoneItemSlot*, HailstoneItem*);
    int (*inspect_item_packet)(void*, const void*, size_t);
    int (*manual_follow)(void*, const char*);
    int (*get_follow_status)(void*, HailstoneFollowStatus*);
    uint32_t (*get_memorized_spell)(void*, uint32_t);
    int (*raid_action)(void*, uint64_t, const char*, uint32_t, uint32_t);
    uint64_t (*monotonic_ms)(void*);
    uint32_t (*world_capabilities)(void*);
    int (*world_floor_below)(void*, uint64_t, uint32_t, float, float, float, float*);
    int (*world_anchor)(void*, uint64_t, uint32_t, uint32_t, HailstoneWorldAnchor*);
    int (*world_submit)(void*, const HailstoneWorldSubmission*);
    int (*world_surface_patch)(void*, const HailstoneWorldSurfacePatch*, HailstoneWorldSurfacePatchResult*);
    int (*measure_binary)(void*, uint32_t, HailstoneBinaryMeasurement*);
    int (*get_map_policy)(void*, HailstoneMapPolicy*);
} FrozenPreviewApi;
/* Frozen public MIT SDK canonical-1 declaration. Keep this independent of the current host
 * declaration and loader compatibility view: the old reader has no raid bulk field. */
typedef struct FrozenCanonical1Api {
    uint32_t abi;
    uint32_t size;
    void* host;
    uint32_t (*caps)(void* host);
    const char* (*name)(void* host);
    void (*log)(void* host, int level, const char* text);

    void (*write_chat)(void* host, const char* text, uint32_t color);
    void (*do_command)(void* host, const char* line);
    int (*add_command)(void* host, const char* name, HailstoneCommandFn fn, void* user);
    void (*remove_command)(void* host, const char* name);

    int (*parse)(void* host, const char* text, char* out, size_t out_size);
    int (*add_tlo)(void* host, const char* name, HailstoneTloFn fn, void* user);
    void (*remove_tlo)(void* host, const char* name);

    int (*send_packet)(void* host, int stream, uint16_t opcode, const void* data, size_t size);
    int (*resize_packet)(void* host, HailstonePacket* p, size_t size);

    int (*config_dir)(void* host, char* out, size_t out_size);
    void* (*imgui_context)(void* host);

    void (*set_active)(void* host, int active);
    void (*set_flag)(void* host, uint32_t flag, int on);
    void (*set_player_classes)(void* host, uint32_t mask);
    void (*set_character_classes)(void* host, const char* name, uint32_t mask);
    void (*set_pet_list)(void* host, const HailstonePet* pets, size_t count);
    void (*set_vitals)(void* host, const HailstoneVitals* v);

    int (*get_player)(void* host, HailstonePlayer* out);
    int (*get_spawn)(void* host, uint32_t spawn_id, HailstoneSpawn* out);
    int (*next_spawn)(void* host, uint32_t after_id, HailstoneSpawn* out);
    int (*get_spell)(void* host, uint32_t spell_id, HailstoneSpell* out);
    int (*get_item)(void* host, const HailstoneItemSlot* slot, HailstoneItem* out);
    int (*get_buff)(void* host, uint32_t index, HailstoneBuff* out);
    int (*get_xtarget)(void* host, uint32_t index, HailstoneXTarget* out);
    int (*eqstr)(void* host, uint32_t id, const char* const* args, size_t count, char* out, size_t out_size);
    int (*right_click_item)(void* host, const HailstoneItemSlot* slot, uint32_t class_id);

    int (*add_ui_function)(void* host, const char* name, HailstoneUiFn fn, void* user);
    void (*remove_ui_function)(void* host, const char* name);
    size_t (*arg_count)(void* host, const HailstoneArgs* args);
    int (*arg)(void* host, const HailstoneArgs* args, size_t index, HailstoneValue* out);
    int (*arg_field)(void* host, const HailstoneArgs* args, size_t index, const char* key, HailstoneValue* out);
    void (*out_value)(void* host, HailstoneOut* out, const HailstoneValue* v);
    void (*out_begin_array)(void* host, HailstoneOut* out);
    void (*out_begin_table)(void* host, HailstoneOut* out);
    void (*out_key)(void* host, HailstoneOut* out, const char* key);
    void (*out_end)(void* host, HailstoneOut* out);
    int (*add_option)(void* host, const char* name, const char* label, int min, int max, HailstoneOptionGetFn get,
                      HailstoneOptionSetFn set, void* user);
    int (*add_menu)(void* host, const char* label, const char* command);

    void (*write_chat_color)(void* host, const char* text, uint32_t color);
    int (*server_config_dir)(void* host, char* out, size_t out_size);
    int (*character_config_dir)(void* host, char* out, size_t out_size);
    int (*zone_name)(void* host, uint32_t zone_id, char* out, size_t out_size);
    void (*close_spell_book)(void* host);
    void (*clear_spell_recovery)(void* host);
    void (*set_window_pet)(void* host, uint32_t spawn_id);
    int (*inspect_item)(void* host, uint32_t item_id, const uint32_t* augments);
    float (*max_camera_distance)(void* host);
    void (*set_max_camera_distance)(void* host, float distance);
    int (*server_info)(void* host, HailstoneServer* out);
    uint32_t (*combat_ability)(void* host, int32_t index);
    int (*hot_button)(void* host, int32_t bar, int32_t page, int32_t button, char* out, size_t out_size);
    int (*set_hot_button)(void* host, int32_t bar, int32_t page, int32_t button, const char* spec);
    int32_t (*hot_bar_page)(void* host, int32_t bar);
    void (*set_hot_bar_page)(void* host, int32_t bar, int32_t page);
    int32_t (*skill)(void* host, uint32_t skill);
    uint32_t (*aa_value)(void* host, int32_t group);

    int32_t (*tcp_open)(void* host, const char* address, uint16_t port);
    int32_t (*tcp_state)(void* host, int32_t handle);
    int64_t (*tcp_send)(void* host, int32_t handle, const void* data, size_t size);
    int64_t (*tcp_recv)(void* host, int32_t handle, void* out, size_t out_size);
    void (*tcp_close)(void* host, int32_t handle);
    int (*tcp_error)(void* host, int32_t handle, char* out, size_t out_size);

    uint32_t (*web_open)(void* host, const char* url, int width, int height);
    int (*web_navigate)(void* host, uint32_t tab, const char* url);
    int (*web_nav)(void* host, uint32_t tab, const char* action);
    void (*web_close)(void* host, uint32_t tab);
    int (*web_eval)(void* host, uint32_t tab, const char* script);
    int (*web_post)(void* host, uint32_t tab, const char* message);
    int (*web_text)(void* host, uint32_t tab, const char* text);
    int (*web_message)(void* host, uint32_t tab, char* out, size_t out_size);
    int (*web_state)(void* host, uint32_t tab, HailstoneWebState* out);
    int (*web_view)(void* host, uint32_t tab, float width, float height);

    int (*open_bank)(void* host, uint32_t banker);
    int32_t (*get_buff_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int32_t (*get_pet_buff_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int32_t (*get_spawns_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    uint32_t (*cursor_origin)(void* host);
    int32_t (*get_xtarget_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int32_t (*get_target_buff_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int32_t (*get_avail_aa_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int32_t (*get_avail_disc_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int32_t (*get_buff_name_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int (*get_raid_roster)(void* host, HailstoneRaidRoster* out);
    int (*get_raid_member)(void* host, uint64_t revision, uint32_t index, HailstoneRaidMember* out);
    int (*select_raid_member)(void* host, uint64_t revision, uint32_t spawn_id, uint64_t incarnation);
    int (*decode_item_packet)(void* host, const void* data, size_t size, HailstoneItemSlot* slot, HailstoneItem* item);
    int (*inspect_item_packet)(void* host, const void* data, size_t size);
    int (*manual_follow)(void* host, const char* action);
    int (*get_follow_status)(void* host, HailstoneFollowStatus* out);
    uint32_t (*get_memorized_spell)(void* host, uint32_t index);
    int (*raid_action)(void* host, uint64_t revision, const char* name, uint32_t action, uint32_t group);
    uint64_t (*monotonic_ms)(void* host);
    uint32_t (*world_capabilities)(void* host);
    int (*world_floor_below)(void* host, uint64_t frame, uint32_t epoch, float x, float y, float seed_z, float* out_z);
    int (*world_anchor)(void* host, uint64_t frame, uint32_t epoch, uint32_t spawn_id, HailstoneWorldAnchor* out);
    int (*world_submit)(void* host, const HailstoneWorldSubmission* submission);
    int (*world_surface_patch)(void* host, const HailstoneWorldSurfacePatch* query,
                               HailstoneWorldSurfacePatchResult* out);
    int (*measure_binary)(void* host, uint32_t kind, HailstoneBinaryMeasurement* out);
    int (*get_map_policy)(void* host, HailstoneMapPolicy* out);
} FrozenCanonical1Api;
/* Frozen canonical-2 adds the raid bulk reader ahead of the same 17 service slots. */
typedef struct FrozenCanonical2Api {
    uint32_t abi;
    uint32_t size;
    void* host;
    uint32_t (*caps)(void* host);
    const char* (*name)(void* host);
    void (*log)(void* host, int level, const char* text);

    void (*write_chat)(void* host, const char* text, uint32_t color);
    void (*do_command)(void* host, const char* line);
    int (*add_command)(void* host, const char* name, HailstoneCommandFn fn, void* user);
    void (*remove_command)(void* host, const char* name);

    int (*parse)(void* host, const char* text, char* out, size_t out_size);
    int (*add_tlo)(void* host, const char* name, HailstoneTloFn fn, void* user);
    void (*remove_tlo)(void* host, const char* name);

    int (*send_packet)(void* host, int stream, uint16_t opcode, const void* data, size_t size);
    int (*resize_packet)(void* host, HailstonePacket* p, size_t size);

    int (*config_dir)(void* host, char* out, size_t out_size);
    void* (*imgui_context)(void* host);

    void (*set_active)(void* host, int active);
    void (*set_flag)(void* host, uint32_t flag, int on);
    void (*set_player_classes)(void* host, uint32_t mask);
    void (*set_character_classes)(void* host, const char* name, uint32_t mask);
    void (*set_pet_list)(void* host, const HailstonePet* pets, size_t count);
    void (*set_vitals)(void* host, const HailstoneVitals* v);

    int (*get_player)(void* host, HailstonePlayer* out);
    int (*get_spawn)(void* host, uint32_t spawn_id, HailstoneSpawn* out);
    int (*next_spawn)(void* host, uint32_t after_id, HailstoneSpawn* out);
    int (*get_spell)(void* host, uint32_t spell_id, HailstoneSpell* out);
    int (*get_item)(void* host, const HailstoneItemSlot* slot, HailstoneItem* out);
    int (*get_buff)(void* host, uint32_t index, HailstoneBuff* out);
    int (*get_xtarget)(void* host, uint32_t index, HailstoneXTarget* out);
    int (*eqstr)(void* host, uint32_t id, const char* const* args, size_t count, char* out, size_t out_size);
    int (*right_click_item)(void* host, const HailstoneItemSlot* slot, uint32_t class_id);

    int (*add_ui_function)(void* host, const char* name, HailstoneUiFn fn, void* user);
    void (*remove_ui_function)(void* host, const char* name);
    size_t (*arg_count)(void* host, const HailstoneArgs* args);
    int (*arg)(void* host, const HailstoneArgs* args, size_t index, HailstoneValue* out);
    int (*arg_field)(void* host, const HailstoneArgs* args, size_t index, const char* key, HailstoneValue* out);
    void (*out_value)(void* host, HailstoneOut* out, const HailstoneValue* v);
    void (*out_begin_array)(void* host, HailstoneOut* out);
    void (*out_begin_table)(void* host, HailstoneOut* out);
    void (*out_key)(void* host, HailstoneOut* out, const char* key);
    void (*out_end)(void* host, HailstoneOut* out);
    int (*add_option)(void* host, const char* name, const char* label, int min, int max, HailstoneOptionGetFn get,
                      HailstoneOptionSetFn set, void* user);
    int (*add_menu)(void* host, const char* label, const char* command);

    void (*write_chat_color)(void* host, const char* text, uint32_t color);
    int (*server_config_dir)(void* host, char* out, size_t out_size);
    int (*character_config_dir)(void* host, char* out, size_t out_size);
    int (*zone_name)(void* host, uint32_t zone_id, char* out, size_t out_size);
    void (*close_spell_book)(void* host);
    void (*clear_spell_recovery)(void* host);
    void (*set_window_pet)(void* host, uint32_t spawn_id);
    int (*inspect_item)(void* host, uint32_t item_id, const uint32_t* augments);
    float (*max_camera_distance)(void* host);
    void (*set_max_camera_distance)(void* host, float distance);
    int (*server_info)(void* host, HailstoneServer* out);
    uint32_t (*combat_ability)(void* host, int32_t index);
    int (*hot_button)(void* host, int32_t bar, int32_t page, int32_t button, char* out, size_t out_size);
    int (*set_hot_button)(void* host, int32_t bar, int32_t page, int32_t button, const char* spec);
    int32_t (*hot_bar_page)(void* host, int32_t bar);
    void (*set_hot_bar_page)(void* host, int32_t bar, int32_t page);
    int32_t (*skill)(void* host, uint32_t skill);
    uint32_t (*aa_value)(void* host, int32_t group);

    int32_t (*tcp_open)(void* host, const char* address, uint16_t port);
    int32_t (*tcp_state)(void* host, int32_t handle);
    int64_t (*tcp_send)(void* host, int32_t handle, const void* data, size_t size);
    int64_t (*tcp_recv)(void* host, int32_t handle, void* out, size_t out_size);
    void (*tcp_close)(void* host, int32_t handle);
    int (*tcp_error)(void* host, int32_t handle, char* out, size_t out_size);

    uint32_t (*web_open)(void* host, const char* url, int width, int height);
    int (*web_navigate)(void* host, uint32_t tab, const char* url);
    int (*web_nav)(void* host, uint32_t tab, const char* action);
    void (*web_close)(void* host, uint32_t tab);
    int (*web_eval)(void* host, uint32_t tab, const char* script);
    int (*web_post)(void* host, uint32_t tab, const char* message);
    int (*web_text)(void* host, uint32_t tab, const char* text);
    int (*web_message)(void* host, uint32_t tab, char* out, size_t out_size);
    int (*web_state)(void* host, uint32_t tab, HailstoneWebState* out);
    int (*web_view)(void* host, uint32_t tab, float width, float height);

    int (*open_bank)(void* host, uint32_t banker);
    int32_t (*get_buff_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int32_t (*get_pet_buff_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int32_t (*get_spawns_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    uint32_t (*cursor_origin)(void* host);
    int32_t (*get_xtarget_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int32_t (*get_target_buff_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int32_t (*get_avail_aa_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int32_t (*get_avail_disc_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int32_t (*get_buff_name_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int32_t (*get_raid_data_bulk)(void* host, unsigned char* out, int32_t out_size);
    int (*get_raid_roster)(void* host, HailstoneRaidRoster* out);
    int (*get_raid_member)(void* host, uint64_t revision, uint32_t index, HailstoneRaidMember* out);
    int (*select_raid_member)(void* host, uint64_t revision, uint32_t spawn_id, uint64_t incarnation);
    int (*decode_item_packet)(void* host, const void* data, size_t size, HailstoneItemSlot* slot, HailstoneItem* item);
    int (*inspect_item_packet)(void* host, const void* data, size_t size);
    int (*manual_follow)(void* host, const char* action);
    int (*get_follow_status)(void* host, HailstoneFollowStatus* out);
    uint32_t (*get_memorized_spell)(void* host, uint32_t index);
    int (*raid_action)(void* host, uint64_t revision, const char* name, uint32_t action, uint32_t group);
    uint64_t (*monotonic_ms)(void* host);
    uint32_t (*world_capabilities)(void* host);
    int (*world_floor_below)(void* host, uint64_t frame, uint32_t epoch, float x, float y, float seed_z, float* out_z);
    int (*world_anchor)(void* host, uint64_t frame, uint32_t epoch, uint32_t spawn_id, HailstoneWorldAnchor* out);
    int (*world_submit)(void* host, const HailstoneWorldSubmission* submission);
    int (*world_surface_patch)(void* host, const HailstoneWorldSurfacePatch* query,
                               HailstoneWorldSurfacePatchResult* out);
    int (*measure_binary)(void* host, uint32_t kind, HailstoneBinaryMeasurement* out);
    int (*get_map_policy)(void* host, HailstoneMapPolicy* out);
} FrozenCanonical2Api;
/* Canonical 3 kept canonical 2's physical declarations and changed only the list payload schema. */
typedef FrozenCanonical2Api FrozenCanonical3Api;

_Static_assert(offsetof(FrozenReleasedApi, get_target_buff_data_bulk) == offsetof(PluginHostApi, get_target_buff_data_bulk), "released prefix moved");
_Static_assert(offsetof(FrozenPreviewApi, get_raid_roster) == sizeof(FrozenCommonApi), "preview prefix moved");
_Static_assert(sizeof(FrozenPreviewApi) == offsetof(PluginHostApi, caps) + 98 * sizeof(host->caps), "preview tail changed");
_Static_assert(offsetof(FrozenReleasedRaidApi, get_raid_data_bulk) == offsetof(PluginHostApi, get_raid_data_bulk), "released raid bulk moved");
_Static_assert(sizeof(FrozenReleasedRaidApi) == offsetof(PluginHostApi, use_ability), "released raid prefix changed");
_Static_assert(offsetof(FrozenReleasedAbilityApi, use_ability) == offsetof(PluginHostApi, use_ability), "released ability action moved");
_Static_assert(offsetof(FrozenReleasedAbilityApi, ability_state) == offsetof(PluginHostApi, ability_state), "released ability state moved");
_Static_assert(offsetof(FrozenReleasedAbilityApi, get_ability_data_bulk) == offsetof(PluginHostApi, get_ability_data_bulk), "released ability bulk moved");
_Static_assert(sizeof(FrozenReleasedAbilityApi) == offsetof(PluginHostApi, get_raid_roster), "released ability prefix changed");
_Static_assert(offsetof(FrozenCanonical1Api, get_raid_roster) == offsetof(FrozenCanonical1Api, caps) + 85 * sizeof(((FrozenCanonical1Api*)0)->caps), "canonical-1 first tail moved");
_Static_assert(offsetof(FrozenCanonical1Api, get_map_policy) == offsetof(FrozenCanonical1Api, caps) + 101 * sizeof(((FrozenCanonical1Api*)0)->caps), "canonical-1 last tail moved");
_Static_assert(sizeof(FrozenCanonical1Api) == offsetof(FrozenCanonical1Api, caps) + 102 * sizeof(((FrozenCanonical1Api*)0)->caps), "canonical-1 tail changed");
_Static_assert(sizeof(FrozenCanonical2Api) == offsetof(FrozenCanonical2Api, caps) + 103 * sizeof(((FrozenCanonical2Api*)0)->caps), "canonical-2 tail changed");
_Static_assert(offsetof(FrozenCanonical2Api, get_raid_roster) == offsetof(FrozenCanonical2Api, caps) + 86 * sizeof(((FrozenCanonical2Api*)0)->caps), "canonical-2 first tail moved");
_Static_assert(offsetof(FrozenCanonical2Api, get_map_policy) == offsetof(FrozenCanonical2Api, caps) + 102 * sizeof(((FrozenCanonical2Api*)0)->caps), "canonical-2 last tail moved");
_Static_assert(sizeof(FrozenCanonical3Api) == offsetof(FrozenCanonical3Api, caps) + 103 * sizeof(((FrozenCanonical3Api*)0)->caps), "canonical-3 tail changed");
#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(FrozenCanonical1Api) == 832, "canonical-1 64-bit size changed");
_Static_assert(offsetof(FrozenCanonical1Api, get_raid_roster) == 696, "canonical-1 64-bit first tail changed");
_Static_assert(offsetof(FrozenCanonical1Api, get_map_policy) == 824, "canonical-1 64-bit last tail changed");
_Static_assert(sizeof(FrozenCanonical2Api) == 840, "canonical-2 64-bit size changed");
_Static_assert(offsetof(FrozenCanonical2Api, get_raid_roster) == 704, "canonical-2 64-bit first tail changed");
_Static_assert(offsetof(FrozenCanonical2Api, get_map_policy) == 832, "canonical-2 64-bit last tail changed");
_Static_assert(sizeof(FrozenCanonical3Api) == 840 && offsetof(FrozenCanonical3Api, get_raid_roster) == 704 && offsetof(FrozenCanonical3Api, get_map_policy) == 832, "canonical-3 64-bit view changed");
#endif

#if HAILSTONE_SDK_PROBE_MODE == 0
HAILSTONE_PLUGIN
#elif (HAILSTONE_SDK_PROBE_MODE == 7 || HAILSTONE_SDK_PROBE_MODE == 8 || HAILSTONE_SDK_PROBE_MODE == 9)
/* Intentionally do not use HAILSTONE_PLUGIN or the current PluginHostApi in negotiation. */
#if HAILSTONE_SDK_PROBE_MODE == 7
typedef FrozenCanonical1Api FrozenMarkedApi;
#define FROZEN_MARKED_LAYOUT UINT64_C(0xf6b34b37061be009)
#elif HAILSTONE_SDK_PROBE_MODE == 8
typedef FrozenCanonical2Api FrozenMarkedApi;
#define FROZEN_MARKED_LAYOUT UINT64_C(0xe7faba7b569803ce)
#else
typedef FrozenCanonical3Api FrozenMarkedApi;
#define FROZEN_MARKED_LAYOUT UINT64_C(0xdbf47b044fcdb052)
#endif
static const FrozenMarkedApi* marked_host;
HAILSTONE_API uint32_t hailstone_plugin_abi(void) { return 3; }
HAILSTONE_API uint64_t hailstone_plugin_layout(void) { return FROZEN_MARKED_LAYOUT; }
HAILSTONE_API int hailstone_plugin_set_host_v2(const FrozenMarkedApi* value, uint64_t layout) {
    setter_called();
    if (layout != FROZEN_MARKED_LAYOUT || !value || value->abi != 3 || value->size != sizeof(*value)) return 0;
    marked_host = value;
    return 1;
}
#else
const PluginHostApi* host;
HAILSTONE_API uint32_t hailstone_plugin_abi(void) { return 3; }
#if HAILSTONE_SDK_PROBE_MODE == 1 || HAILSTONE_SDK_PROBE_MODE == 2 || HAILSTONE_SDK_PROBE_MODE == 4
HAILSTONE_API uint64_t hailstone_plugin_layout(void) {
#if HAILSTONE_SDK_PROBE_MODE == 1
    return UINT64_MAX;
#else
    return HAILSTONE_PLUGIN_LAYOUT;
#endif
}
#endif
#if HAILSTONE_SDK_PROBE_MODE == 2 || HAILSTONE_SDK_PROBE_MODE == 5 || HAILSTONE_SDK_PROBE_MODE == 6
HAILSTONE_API void hailstone_plugin_set_host(const PluginHostApi* value) { setter_called(); host = value; }
#else
HAILSTONE_API int hailstone_plugin_set_host_v2(const PluginHostApi* value, uint64_t layout) {
    (void)layout;
    setter_called(); host = value;
    return HAILSTONE_SDK_PROBE_MODE != 4 && layout == HAILSTONE_PLUGIN_LAYOUT;
}
#endif
#endif

static const FrozenCommonApi* common_api(void) {
#if (HAILSTONE_SDK_PROBE_MODE == 7 || HAILSTONE_SDK_PROBE_MODE == 8 || HAILSTONE_SDK_PROBE_MODE == 9)
    return (const FrozenCommonApi*)marked_host;
#else
    return (const FrozenCommonApi*)host;
#endif
}
static void state(void* user, const char* arguments) {
    const FrozenCommonApi* common = common_api();
    char text[256];
    (void)user; (void)arguments;
    snprintf(text, sizeof(text), "[SDK] table size %u", common->size);
    common->write_chat(common->host, text, 0);
}
#if HAILSTONE_SDK_PROBE_MODE == 5
static void released(void* user, const char* arguments) {
    const FrozenReleasedApi* api = (const FrozenReleasedApi*)host;
    unsigned char bytes[4096];
    char text[256];
    int target, aa, disc, buffs;
    int raid;
    const FrozenReleasedRaidApi* extended = (const FrozenReleasedRaidApi*)host;
    const FrozenReleasedAbilityApi* abilities = (const FrozenReleasedAbilityApi*)host;
    int64_t remaining = -2, reuse = -2;
    int ready, used, ability_bytes;
    (void)user; (void)arguments;
    target = api->get_target_buff_data_bulk(api->common.host, bytes, (int32_t)sizeof(bytes));
    aa = api->get_avail_aa_data_bulk(api->common.host, bytes, (int32_t)sizeof(bytes));
    disc = api->get_avail_disc_data_bulk(api->common.host, bytes, (int32_t)sizeof(bytes));
    buffs = api->get_buff_name_data_bulk(api->common.host, bytes, (int32_t)sizeof(bytes));
    raid = extended->get_raid_data_bulk(api->common.host, bytes, (int32_t)sizeof(bytes));
    snprintf(text, sizeof(text), "[SDK] released readers %d %d %d %d %d", target >= 0, aa >= 4, disc >= 4, buffs > 0,
             raid >= HS_BULK_RAID_HEADER_SIZE);
    api->common.write_chat(api->common.host, text, 0);
    ready = abilities->ability_state(api->common.host, 0, "Kick", &remaining, &reuse);
    used = abilities->use_ability(api->common.host, 0, "Kick");
    ability_bytes = abilities->get_ability_data_bulk(api->common.host, bytes, (int32_t)sizeof(bytes));
    snprintf(text, sizeof(text), "[SDK] released abilities %d %lld %lld %d %d", ready,
             (long long)remaining, (long long)reuse, used, ability_bytes);
    api->common.write_chat(api->common.host, text, 0);
}
#endif
#if HAILSTONE_SDK_PROBE_MODE == 6
static void preview(void* user, const char* arguments) {
    const FrozenPreviewApi* api = (const FrozenPreviewApi*)host;
    HailstoneRaidRoster roster = {0};
    HailstoneMapPolicy policy = {0};
    HailstoneBinaryMeasurement binary = {0};
    char text[256];
    int raid, map, measured;
    (void)user; (void)arguments;
    roster.size = sizeof(roster); policy.size = 72; binary.size = sizeof(binary);
    raid = api->get_raid_roster(api->common.host, &roster);
    map = api->get_map_policy(api->common.host, &policy);
    measured = api->measure_binary(api->common.host, HAILSTONE_BINARY_CALLING_PLUGIN, &binary);
    snprintf(text, sizeof(text), "[SDK] preview services %d %d %d %d", raid, map, measured, api->monotonic_ms(api->common.host) != 0);
    api->common.write_chat(api->common.host, text, 0);
}
#endif
#if (HAILSTONE_SDK_PROBE_MODE == 7 || HAILSTONE_SDK_PROBE_MODE == 8 || HAILSTONE_SDK_PROBE_MODE == 9)
static unsigned char item_bytes[262144];
static size_t item_bytes_size;
HAILSTONE_API int OnPacket(int stream, int direction, HailstonePacket* packet) {
    (void)stream; (void)direction;
    item_bytes_size = 0;
    if (packet && packet->data && packet->size && packet->size <= sizeof(item_bytes)) {
        memcpy(item_bytes, packet->data, packet->size);
        item_bytes_size = packet->size;
    }
    return 0;
}
static void marked(void* user, const char* arguments) {
    HailstoneRaidRoster roster = {0};
    HailstoneMapPolicy policy = {0};
    HailstoneBinaryMeasurement binary = {0};
    char text[256];
    int raid, map, measured, timed;
    uint32_t world;
    (void)user; (void)arguments;
    roster.size = sizeof(roster); policy.size = sizeof(policy); binary.size = sizeof(binary);
    raid = marked_host->get_raid_roster(marked_host->host, &roster);
    map = marked_host->get_map_policy(marked_host->host, &policy);
    measured = marked_host->measure_binary(marked_host->host, HAILSTONE_BINARY_CALLING_PLUGIN, &binary);
    timed = marked_host->monotonic_ms(marked_host->host) != 0;
    world = marked_host->world_capabilities(marked_host->host);
    snprintf(text, sizeof(text), "[SDK] marked services %d %d %d %d world %u", raid, map, measured, timed, world);
    marked_host->write_chat(marked_host->host, text, 0);
}
static void marked_item(void* user, const char* arguments) {
    HailstoneItem item;
    HailstoneItemSlot slot;
    unsigned char previous_item[sizeof(item)], previous_slot[sizeof(slot)];
    char text[256];
    int result;
    (void)user;
    memset(&item, 0x5a, sizeof(item)); memset(&slot, 0x5a, sizeof(slot));
    item.size = arguments && *arguments ? (uint32_t)strtoul(arguments, 0, 10) : (uint32_t)sizeof(item);
    memcpy(previous_item, &item, sizeof(item)); memcpy(previous_slot, &slot, sizeof(slot));
    result = marked_host->decode_item_packet(marked_host->host, item_bytes, item_bytes_size, &slot, &item);
    snprintf(text, sizeof(text), "[SDK] marked decode %d slot_unchanged %d item_unchanged %d id %u", result,
             memcmp(previous_slot, &slot, sizeof(slot)) == 0, memcmp(previous_item, &item, sizeof(item)) == 0, item.id);
    marked_host->write_chat(marked_host->host, text, 0);
}
/* Frozen payload readers, independent of this SDK's current declarations. Canonical 3 uses count/total. */
static void old_lists(void* user, const char* arguments) {
    unsigned char bytes[1040];
    unsigned requested = 1024, guard = 0, records = 0;
    char kind[16] = "target", text[256];
    int32_t result = 0, count = -1, first = -1;
#if HAILSTONE_SDK_PROBE_MODE == 9
    const size_t header_size = 8;
    int32_t total = -1;
#else
    const size_t header_size = 4;
#endif
    size_t at = header_size, end = 0, index, fixed;
    (void)user;
    if (arguments && *arguments) {
        size_t length = strcspn(arguments, " \t");
        const char* number = arguments + length;
        if (length >= sizeof(kind)) length = sizeof(kind) - 1;
        memcpy(kind, arguments, length); kind[length] = '\0';
        while (*number == ' ' || *number == '\t') ++number;
        if (*number) requested = (unsigned)strtoul(number, 0, 10);
    }
    fixed = strcmp(kind, "target") == 0 ? 20u : 8u;
    if (requested > 1024) requested = 1024;
    memset(bytes, 0xa5, sizeof(bytes));
    if (strcmp(kind, "target") == 0)
        result = marked_host->get_target_buff_data_bulk(marked_host->host, bytes, (int32_t)requested);
    else if (strcmp(kind, "aa") == 0)
        result = marked_host->get_avail_aa_data_bulk(marked_host->host, bytes, (int32_t)requested);
    else if (strcmp(kind, "disc") == 0)
        result = marked_host->get_avail_disc_data_bulk(marked_host->host, bytes, (int32_t)requested);
    for (index = requested; index < sizeof(bytes); ++index) if (bytes[index] != 0xa5) ++guard;
    if (result > 0 && (size_t)result <= requested) end = (size_t)result;
    if (end >= header_size) {
        memcpy(&count, bytes, sizeof(count));
#if HAILSTONE_SDK_PROBE_MODE == 9
        memcpy(&total, bytes + 4, sizeof(total));
#endif
        while (at <= end && fixed <= end - at) {
            int32_t id = 0, length = 0;
            memcpy(&id, bytes + at, sizeof(id));
            memcpy(&length, bytes + at + fixed - 4, sizeof(length));
            if (length < 0 || (size_t)length > end - at - fixed) break;
            if (!records) first = id;
            ++records; at += fixed + (size_t)length;
        }
    } else at = end;
#if HAILSTONE_SDK_PROBE_MODE == 9
    snprintf(text, sizeof(text), "[SDK] retained8 %s bytes %d count %d total %d records %u first %d guard %u left %zu",
             kind, result, count, total, records, first, guard, end - at);
#else
    snprintf(text, sizeof(text), "[SDK] old %s bytes %d count %d records %u first %d guard %u left %zu",
             kind, result, count, records, first, guard, end - at);
#endif
    marked_host->write_chat(marked_host->host, text, 0);
}
static void marked_world(void* user, const char* arguments) {
    unsigned long long frame = 0;
    unsigned epoch = 0, spawn = 0;
    HailstoneWorldAnchor anchor = {0};
    HailstoneWorldSurfacePatch query = {0};
    HailstoneWorldSurfacePatchResult patch = {0};
    HailstoneWorldVertex vertices[3] = {{{0, 0, 0}, {1, 1, 1, 1}}, {{1, 0, 0}, {1, 1, 1, 1}}, {{0, 1, 0}, {1, 1, 1, 1}}};
    HailstoneWorldSubmission submission = {0};
    char text[256];
    float floor_z = 123.0f;
    uint32_t capabilities;
    int floor_result, anchor_result, patch_result, submitted;
    (void)user;
    if (arguments) {
        char* end;
        frame = strtoull(arguments, &end, 10);
        epoch = (unsigned)strtoul(end, &end, 10);
        spawn = (unsigned)strtoul(end, 0, 10);
    }
    anchor.size = sizeof(anchor); query.size = sizeof(query); patch.size = sizeof(patch);
    query.frame = (uint64_t)frame; query.epoch = epoch;
    query.vertices[3] = 1; query.vertices[7] = 1; query.maximum_rise = 1; query.tolerance = 0.01f;
    submission.size = sizeof(submission); submission.frame = (uint64_t)frame; submission.epoch = epoch;
    submission.tag = 1; submission.revision = 1; submission.primitive = HAILSTONE_WORLD_TRIANGLES;
    submission.vertices = vertices; submission.vertex_count = 3;
    capabilities = marked_host->world_capabilities(marked_host->host);
    floor_result = marked_host->world_floor_below(marked_host->host, (uint64_t)frame, epoch, 0, 0, 10, &floor_z);
    anchor_result = marked_host->world_anchor(marked_host->host, (uint64_t)frame, epoch, spawn, &anchor);
    patch_result = marked_host->world_surface_patch(marked_host->host, &query, &patch);
    submitted = marked_host->world_submit(marked_host->host, &submission);
    snprintf(text, sizeof(text), "[SDK] marked world %u %d %d %d %d floor %.3f anchor %u", capabilities,
             floor_result, anchor_result, patch_result, submitted, (double)floor_z, anchor.spawn_id);
    marked_host->write_chat(marked_host->host, text, 0);
}
static void marked_world_frame(const HailstoneWorldFrame* frame) {
    char arguments[96];
    if (!frame) return;
    snprintf(arguments, sizeof(arguments), "%llu %u 12", (unsigned long long)frame->frame, frame->epoch);
    marked_world(0, arguments);
}
HAILSTONE_API const HailstoneHooks* hailstone_plugin_hooks(void) {
    static HailstoneHooks hooks;
    hooks.size = sizeof(hooks);
    hooks.world_frame = marked_world_frame;
    return &hooks;
}
#endif
HAILSTONE_API void InitializePlugin(void) {
    const FrozenCommonApi* common = common_api();
    if (initialize_calls) ++*initialize_calls;
    if (!common) return;
    common->add_command(common->host, "/sdkstate", state, 0);
#if HAILSTONE_SDK_PROBE_MODE == 5
    common->add_command(common->host, "/sdkreleased", released, 0);
#elif HAILSTONE_SDK_PROBE_MODE == 6
    common->add_command(common->host, "/sdkpreview", preview, 0);
#elif (HAILSTONE_SDK_PROBE_MODE == 7 || HAILSTONE_SDK_PROBE_MODE == 8 || HAILSTONE_SDK_PROBE_MODE == 9)
    common->add_command(common->host, "/sdkmarked", marked, 0);
    common->add_command(common->host, "/sdkmarkeditem", marked_item, 0);
    common->add_command(common->host, "/sdkoldlists", old_lists, 0);
    common->add_command(common->host, "/sdkmarkedworld", marked_world, 0);
#endif
    common->write_chat(common->host, "[SDK] initialized", 0);
}
