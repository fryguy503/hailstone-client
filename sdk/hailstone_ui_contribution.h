/* SPDX-License-Identifier: MIT (sdk/LICENSE) */
#ifndef HAILSTONE_UI_CONTRIBUTION_H
#define HAILSTONE_UI_CONTRIBUTION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HAILSTONE_UI_CONTRIBUTION_DISABLED (1u << 0)

/* A notice or disabled control in a location supported by the UI. The host copies
 * all strings before returning. IDs, windows and targets are case-sensitive.
 * A disabled control requires a target. Omit the text to use a stock explanation. */
typedef struct HailstoneUiContribution {
    uint32_t size;          /* sizeof(HailstoneUiContribution) used by the caller */
    uint32_t flags;         /* HAILSTONE_UI_CONTRIBUTION_* */
    const char* id;         /* stable ID within the calling addon */
    const char* window;     /* e.g. OptionsWindow */
    const char* target;     /* a location in the window, NULL or empty for a window notice */
    const char* text;       /* plain text shown to the player */
    const char* action_label; /* optional button label, paired with command */
    const char* command;    /* optional slash command, executed as the addon on player click */
} HailstoneUiContribution;

#ifdef __cplusplus
}
#endif

#endif
