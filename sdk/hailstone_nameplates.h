/* SPDX-License-Identifier: MIT (sdk/LICENSE) */
#ifndef HAILSTONE_NAMEPLATES_H
#define HAILSTONE_NAMEPLATES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Generic world nameplate canvas. Borrowed text and frame records survive only
 * until the callback returns. Positions and bounds use window pixels. Candidate
 * indexes are zero-based drawing claims scoped to the frame and callback.
 * Candidate and command array element layouts are fixed for this API. */
typedef struct HailstoneNameplateBounds {
    float x, y, width, height;
} HailstoneNameplateBounds;

typedef struct HailstoneNameplateString {
    const char* data;
    uint32_t bytes;              /* without a trailing NUL */
} HailstoneNameplateString;

enum HailstoneNameplateCandidateFlag {
    HAILSTONE_NAMEPLATE_NPC = 1u << 0,
    HAILSTONE_NAMEPLATE_CORPSE = 1u << 1,
    HAILSTONE_NAMEPLATE_MERCENARY = 1u << 2,
    HAILSTONE_NAMEPLATE_GM = 1u << 3,
    HAILSTONE_NAMEPLATE_TARGET = 1u << 4,
    HAILSTONE_NAMEPLATE_SELF = 1u << 5,
    HAILSTONE_NAMEPLATE_GROUP = 1u << 6,
    HAILSTONE_NAMEPLATE_RAID = 1u << 7,
    HAILSTONE_NAMEPLATE_HP_KNOWN = 1u << 8,
    HAILSTONE_NAMEPLATE_MANA_KNOWN = 1u << 9,
    HAILSTONE_NAMEPLATE_ENDURANCE_KNOWN = 1u << 10,
    HAILSTONE_NAMEPLATE_AGGRO_KNOWN = 1u << 11,
    HAILSTONE_NAMEPLATE_CASTING = 1u << 12,
    HAILSTONE_NAMEPLATE_CONCEALED = 1u << 13,
    HAILSTONE_NAMEPLATE_INVISIBLE = 1u << 14,
    HAILSTONE_NAMEPLATE_INVISIBLE_UNDEAD = 1u << 15
};

typedef struct HailstoneNameplateCandidate {
    uint32_t flags;
    uint32_t spawn_id;
    uint64_t incarnation;
    uint32_t level, class_id, race, owner_id, guild_id, rider_id, body_type;
    uint32_t group_mark, raid_mark; /* 0 when none */
    uint32_t cast_spell_id;
    float x, y, depth, native_scale;
    float world_x, world_y, world_z, heading;
    double distance;
    double hp_percent, mana_percent, endurance_percent, aggro_percent; /* test the matching KNOWN flag */
    double cast_elapsed_ms, cast_duration_ms; /* present when CASTING is set */
    HailstoneNameplateString spawn_name, caption, cast_name;
} HailstoneNameplateCandidate;

typedef struct HailstoneNameplateFrame {
    uint32_t size;
    uint32_t candidate_count;
    uint64_t frame;
    double now_ms, dt_ms;
    float font_size;
    int32_t font;
    HailstoneNameplateBounds viewport;
    const HailstoneNameplateCandidate* candidates; /* target first, then increasing depth */
} HailstoneNameplateFrame;

typedef void (*HailstoneNameplateFn)(void* userdata, const HailstoneNameplateFrame* frame);

enum HailstoneNameplateCommandKind {
    HAILSTONE_NAMEPLATE_RECT = 0,
    HAILSTONE_NAMEPLATE_RECT_FILLED = 1,
    HAILSTONE_NAMEPLATE_GRADIENT = 2,
    HAILSTONE_NAMEPLATE_LINE = 3,
    HAILSTONE_NAMEPLATE_CIRCLE = 4,
    HAILSTONE_NAMEPLATE_CIRCLE_FILLED = 5,
    HAILSTONE_NAMEPLATE_TRIANGLE = 6,
    HAILSTONE_NAMEPLATE_TRIANGLE_FILLED = 7,
    HAILSTONE_NAMEPLATE_TEXT = 8,
    HAILSTONE_NAMEPLATE_IMAGE = 9
};

/* Set unused fields to zero. Coordinates are relative to the candidate head.
 * Color is 0xRRGGBB with separate alpha in [0,1]. A gradient uses both colors
 * and alpha values. Images take an opaque texture returned by spell_icon in
 * the current callback. Text takes counted UTF-8 without control characters. */
typedef struct HailstoneNameplateCommand {
    uint32_t kind;
    uint32_t color, color2;
    float alpha, alpha2;
    float x, y, x0, y0, x1, y1, x2, y2;
    float radius, rounding, thickness;
    float font_size;
    int32_t font, segments;
    uintptr_t texture;
    float u0, v0, u1, v1;
    HailstoneNameplateString text;
} HailstoneNameplateCommand;

enum HailstoneNameplateModelFlag {
    HAILSTONE_NAMEPLATE_HIDE = 1u << 0,
    HAILSTONE_NAMEPLATE_CLICKABLE = 1u << 1,
    HAILSTONE_NAMEPLATE_THROUGH_WALLS = 1u << 2
};

typedef struct HailstoneNameplateModel {
    uint32_t size;                /* sizeof(HailstoneNameplateModel) */
    uint32_t flags;
    uint64_t frame;
    uint32_t candidate_index;
    uint32_t command_count;
    HailstoneNameplateBounds bounds;
    float offset_x, offset_y;
    const HailstoneNameplateCommand* commands; /* copied during submit */
} HailstoneNameplateModel;

typedef struct HailstoneNameplateTextMetrics {
    uint32_t size;                /* initialize to sizeof(HailstoneNameplateTextMetrics) */
    float width, height;
    float ink_x0, ink_y0, ink_x1, ink_y1;
} HailstoneNameplateTextMetrics;

typedef struct HailstoneNameplateImage {
    uint32_t size;                /* initialize to sizeof(HailstoneNameplateImage) */
    uintptr_t texture;
    float u0, v0, u1, v1;
} HailstoneNameplateImage;

#ifdef __cplusplus
}
#endif

#endif
