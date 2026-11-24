/* SPDX-License-Identifier: MIT (sdk/LICENSE) */
/* A plugin built against the SDK header 0.7.1-beta shipped (tests/plugins/sdk071/hailstone_plugin.h, a frozen copy):
 * the newest released ABI 3 plugins export hailstone_plugin_set_host / nmsc_plugin_set_host and know the host table
 * up to get_xtarget_data_bulk. tests/native_plugins_test.cpp loads it in this client, so a change to the loader or a
 * reordered table that would break such a plugin fails there. /sdk071 prints what it sees through the table. */
#include <stdio.h>

#include "hailstone_plugin.h"

HAILSTONE_PLUGIN

static void sdk071(void* user, const char* args) {
    unsigned char buf[512];
    char name[64] = "";
    (void)user;
    (void)args;
    int32_t xtarget = -1;
    if (HAILSTONE_HOST_HAS(host, get_xtarget_data_bulk)) xtarget = host->get_xtarget_data_bulk(host->host, buf, 512);
    snprintf(name, sizeof name, "%s", host->name(host->host));
    WriteChatf("[Sdk071] abi %u table %d name %s xtarget %d", (unsigned)host->abi,
               host->size >= sizeof(PluginHostApi) ? 1 : 0, name, (int)(xtarget % HS_BULK_XTARGET_RECORD_SIZE));
}

HAILSTONE_API void InitializePlugin(void) {
    AddCommand("/sdk071", sdk071, 0);
    WriteChatf("[Sdk071] loaded");
}

HAILSTONE_API void ShutdownPlugin(void) { RemoveCommand("/sdk071"); }

/* Built for ABI 3, before Dear ImGui 1.92: the client must not call this (it would draw into a context of another
 * layout). */
HAILSTONE_API void OnUpdateImGui(void) { WriteChatf("[Sdk071] OnUpdateImGui called"); }
