/* SPDX-License-Identifier: MIT (sdk/LICENSE) */
#ifndef HAILSTONE_WORLD_GEOMETRY_H
#define HAILSTONE_WORLD_GEOMETRY_H

/* Generic, transient world rendering. Coordinates use the client's zone-file
 * axes, as HailstoneSpawn does. EQEmu packet X/Y are swapped relative to these
 * axes; a protocol addon converts at its boundary. The host never swaps them.
 * No game-server protocol is implied. */
#include <stddef.h>
#include <stdint.h>

enum HailstoneWorldCapability {
    HAILSTONE_WORLD_GEOMETRY = 1u,
    HAILSTONE_WORLD_TERRAIN = 2u,
    HAILSTONE_WORLD_DRAWN_ANCHORS = 4u,
    HAILSTONE_WORLD_PRESENT_RECEIPTS = 8u,
    HAILSTONE_WORLD_SURFACE_PATCH = 16u,
    HAILSTONE_WORLD_PLACEMENT = 32u /* world_placement_* supported; reported in the zone without a render frame */
};
enum HailstoneWorldQueryResult {
    HAILSTONE_WORLD_DEFERRED = 0,  /* unavailable scene geometry, stale frame, or exhausted query budget */
    HAILSTONE_WORLD_MISSING = 2,   /* query completed, no surface/anchor */
    HAILSTONE_WORLD_RESOLVED = 1
};
enum HailstoneWorldPrimitive {
    HAILSTONE_WORLD_TRIANGLES = 0, /* vertex count is a multiple of three */
    HAILSTONE_WORLD_LINES = 1      /* vertex count is a multiple of two; one pixel wide */
};
enum HailstoneWorldPlacementStatus {
    HAILSTONE_WORLD_PLACEMENT_INACTIVE = 0,
    HAILSTONE_WORLD_PLACEMENT_AIMING_INVALID = 1,
    HAILSTONE_WORLD_PLACEMENT_AIMING_VALID = 2,
    HAILSTONE_WORLD_PLACEMENT_CONFIRMED = 3,
    HAILSTONE_WORLD_PLACEMENT_CANCELLED = 4
};

typedef struct HailstoneWorldPlacement {
    uint32_t size;
    uint32_t status;          /* HailstoneWorldPlacementStatus */
    uint32_t zone_id;
    uint32_t instance_id;
    float position[3];        /* zone-file axes, valid for AIMING_VALID and CONFIRMED */
} HailstoneWorldPlacement;
enum HailstoneWorldAnchorFlag {
    HAILSTONE_WORLD_DRAWN_POSE = 1u,
    HAILSTONE_WORLD_CACHED_FLOOR = 2u
};
enum HailstoneWorldReceiptFlag {
    HAILSTONE_WORLD_RECORDED = 1u,
    HAILSTONE_WORLD_PRESENTED = 2u,
    HAILSTONE_WORLD_SUBMITTED = 4u
};
enum HailstoneWorldReceiptStatus {
    HAILSTONE_WORLD_FRAME_UNAVAILABLE = 0,
    HAILSTONE_WORLD_BATCH_FINISHED = 1,
    HAILSTONE_WORLD_FRAME_FINISHED = 2 /* tag zero; follows every batch receipt for this owner/frame */
};

typedef struct HailstoneWorldFrame {
    uint32_t size;
    uint32_t capabilities;
    uint64_t frame;             /* nonzero, host-issued render-attempt token */
    uint32_t epoch;             /* changes on world/zone retirement */
    uint32_t collision_revision;
    uint64_t now_ms;            /* monotonic milliseconds, never wall time */
    uint32_t maximum_batches;
    uint32_t maximum_vertices;
    uint32_t remaining_queries;
    float camera_position[3];
    float viewport[4];          /* x, y, width, height */
    float view_projection[16];  /* column-major; maps zone-file coordinates to clip coordinates */
} HailstoneWorldFrame;

typedef struct HailstoneWorldAnchor {
    uint32_t size;
    uint32_t spawn_id;
    uint32_t flags;
    float position[3];          /* current render-model origin, including intentionally hidden first-person actors;
                                * never a stale/network-position fallback, and not a visibility assertion */
    float heading;              /* EQ heading units, 512 per turn */
    float floor_z;              /* meaningful only with CACHED_FLOOR */
} HailstoneWorldAnchor;

enum HailstoneWorldSurfacePatchStatus {
    HAILSTONE_WORLD_PATCH_DEFERRED = 0, /* unavailable, stale, invalid or incomplete within the work budget */
    HAILSTONE_WORLD_PATCH_PLANAR = 1,   /* complete footprint covered within tolerance, no higher bounded face */
    HAILSTONE_WORLD_PATCH_MISMATCH = 2, /* witness on a different current surface */
    HAILSTONE_WORLD_PATCH_MISSING = 3   /* witness of a coverage hole */
};
enum HailstoneWorldSurfacePatchFlag {
    HAILSTONE_WORLD_PATCH_HEIGHT_KNOWN = 1u,
    HAILSTONE_WORLD_PATCH_SOURCE_FACE = 2u,
    HAILSTONE_WORLD_PATCH_REGION = 4u
};
enum { HAILSTONE_WORLD_PATCH_MAX_REGION_VERTICES = 32 };

typedef struct HailstoneWorldSurfacePatch {
    uint32_t size;
    uint32_t epoch;
    uint64_t frame;
    float vertices[9];         /* three XYZ floor vertices defining a nondegenerate XY triangle/reference plane */
    float maximum_rise;        /* local ceiling = interpolated reference plane + this nonnegative offset */
    float tolerance;           /* nonnegative permitted vertical difference from that plane */
} HailstoneWorldSurfacePatch;

typedef struct HailstoneWorldSurfacePatchResult {
    uint32_t size;
    uint32_t flags;
    float witness[3];          /* MISMATCH/MISSING: interior XY; a float-rounded noninterior point requires both
                               * SOURCE_FACE and a positive-area REGION with a strictly interior double centroid,
                               * and XY must exactly equal float(centroid). With REGION the height/source face
                               * are resolved at that double centroid, before float rounding.
                               * Z is meaningful only with HEIGHT_KNOWN. */
    float source_face[9];      /* SOURCE_FACE: actual selected drawn triangle, three world-space XYZ vertices;
                               * contains the resolved witness/region centroid. Re-certify any derived patches. */
    uint32_t region_count;
    double region_xy[HAILSTONE_WORLD_PATCH_MAX_REGION_VERTICES * 2];
                               /* REGION: bounded convex refinement region inside the input XY triangle;
                                * higher-face overlap or selected uncovered piece, in world axes. This is a
                                * refinement hint, never a certificate. Re-certify every derived triangle. */
} HailstoneWorldSurfacePatchResult;

typedef struct HailstoneWorldVertex {
    float position[3];
    float color[4];             /* display sRGB, straight alpha, finite channels in [0,1] */
} HailstoneWorldVertex;

typedef struct HailstoneWorldSubmission {
    uint32_t size;
    uint32_t epoch;
    uint64_t frame;
    uint64_t tag;               /* nonzero producer-owned identity, scoped to its host owner */
    uint64_t revision;
    uint32_t primitive;
    const HailstoneWorldVertex* vertices;
    size_t vertex_count;        /* copied immediately; one entire batch accepted or rejected */
} HailstoneWorldSubmission;

typedef struct HailstoneWorldReceipt {
    uint32_t size;
    uint32_t status;
    uint32_t epoch;
    uint32_t flags;
    uint64_t frame;
    uint64_t tag;
    uint64_t revision;
    uint32_t vertex_count;
    /* PRESENTED means the complete recorded batch belonged to a successfully
     * accepted presentation. It does not prove monitor pixels, visibility or
     * player attention. No receipt authorizes a game action. A tag-zero
     * FRAME_UNAVAILABLE notification retires previous frame availability.
     * FRAME_FINISHED carries the frame's submission/presentation outcome;
     * consumers may then combine its preceding per-batch receipts atomically. */
} HailstoneWorldReceipt;

#endif
