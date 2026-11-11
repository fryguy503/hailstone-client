#ifndef HAILSTONE_BUILTIN_ADDON_H
#define HAILSTONE_BUILTIN_ADDON_H

/* A statically linked add-on uses the public host ABI without loading a shared library.
 * The descriptor and all callbacks belong to the linked module for the process lifetime.
 * Managed clients accept read/events/UI/packets/extend/login capabilities only; no command,
 * movement, targeting, action, chat or independent network automation is granted.
 */
#include "hailstone_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HailstoneBuiltinAddon {
    uint32_t size;
    uint32_t abi;
    const char* name;
    uint32_t capabilities;
    void (*set_host)(const PluginHostApi*);
    void (*initialize)(void);
    void (*shutdown)(void);
    void (*pulse)(void);
    void (*game_state)(int);
    void (*begin_zone)(void);
    void (*end_zone)(void);
    void (*zoned)(void);
    void (*add_spawn)(uint32_t);
    void (*remove_spawn)(uint32_t);
    int (*incoming_chat)(const char*, uint32_t);
    int (*filter_chat)(const char*, uint32_t);
    void (*update_imgui)(void);
    int (*packet)(int, int, HailstonePacket*);
    const HailstoneHooks* (*hooks)(void);
} HailstoneBuiltinAddon;

/* Defined by the distribution's separately maintained, statically linked module. */
const HailstoneBuiltinAddon* hailstone_builtin_addon(void);

#ifdef __cplusplus
}
#endif
#endif
