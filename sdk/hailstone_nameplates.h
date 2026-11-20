/* SPDX-License-Identifier: MIT (sdk/LICENSE) */
#ifndef HAILSTONE_NAMEPLATES_H
#define HAILSTONE_NAMEPLATES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Generic world nameplate canvas. Borrowed text and frame records survive only
 * until the callback returns. Positions and bounds use window pixels. Candidate
 * indexes are zero-based drawing claims scoped to the frame and callback.
 * The candidate and command arrays carry their element size (stride), so either
 * record may grow at its end within ABI 3: step through candidates by the
 * host's candidate_size (hailstone_nameplate_candidate), and tell the host the
 * size of the command records you pass in command_size. */
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
    HailstoneNameplateString secondary_caption; /* optional ABI 3 tail: stock guild or NPC description line, if visible */
} HailstoneNameplateCandidate;

typedef struct HailstoneNameplateFrame {
    uint32_t size;
    uint32_t candidate_count;
    uint32_t candidate_size;      /* the host's sizeof(HailstoneNameplateCandidate): the array's stride */
    uint32_t reserved;            /* 0 */
    uint64_t frame;
    double now_ms, dt_ms;
    float font_size;
    int32_t font;
    HailstoneNameplateBounds viewport;
    const void* candidates;       /* candidate_count records candidate_size apart: target first, then increasing depth */
} HailstoneNameplateFrame;

/* Candidate `index` of the frame, stepping by the host's stride; NULL past the end. A field this SDK has and an
 * older client lacks is there only when HAILSTONE_NAMEPLATE_CANDIDATE_HAS(frame, field) says so. */
static inline const HailstoneNameplateCandidate* hailstone_nameplate_candidate(const HailstoneNameplateFrame* frame,
                                                                               uint32_t index) {
    if (!frame || !frame->candidates || index >= frame->candidate_count) return NULL;
    return (const HailstoneNameplateCandidate*)((const unsigned char*)frame->candidates +
                                                (size_t)index * frame->candidate_size);
}
#define HAILSTONE_NAMEPLATE_CANDIDATE_HAS(frame, field)                                                             \
    ((frame)->candidate_size >= offsetof(HailstoneNameplateCandidate, field) +                                      \
                                    sizeof(hailstone_nameplate_candidate((frame), 0)->field))

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
 * the current callback. Text takes counted UTF-8 without control characters.
 * A host reads the fields it knows from a larger record (a newer SDK's) and
 * refuses a model whose command_size is smaller than its own record. */
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
    uint32_t command_size;        /* sizeof(HailstoneNameplateCommand): the commands array's stride */
    uint32_t reserved;            /* 0 */
    HailstoneNameplateBounds bounds;
    float offset_x, offset_y;
    const void* commands;         /* command_count records command_size apart; copied during submit */
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
