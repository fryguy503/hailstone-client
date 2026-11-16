#ifndef HAILSTONE_FOLLOW_H
#define HAILSTONE_FOLLOW_H

#include <stdint.h>

/* Data-only movement profiles. A profile changes a user's manually started
 * follow; it cannot choose a target, inject keys, navigate, or start a walk.
 * Queries require EXTEND and an active module. The host validates every field.
 * Callers initialize size; providers return 0 to keep stock behavior. */
#define HAILSTONE_FOLLOW_ALLOW_UNGROUPED 1u
#define HAILSTONE_FOLLOW_CANCEL_ON_MOVE 2u
#define HAILSTONE_FOLLOW_LOOK_PITCH 4u
#define HAILSTONE_FOLLOW_PRESERVE_RUN 8u
typedef struct HailstoneFollowProfile {
    uint32_t size;
    uint32_t flags;
    float stop_distance;       /* 1..50 planar zone units, independent of model height */
    float restart_margin;      /* 0..10; stopped follows resume beyond distance + margin */
    float start_range;         /* 1..200; manual activation range */
    float turn_snap;           /* 0..16 EQ heading units */
    float turn_slow, turn_fast;/* 1..32 heading units per 1/60 second */
    float turn_fast_angle;     /* 0..256 */
    float move_angle;          /* 0..256; face this close before moving */
    float move_far_margin;     /* 0..50; a distant target permits movement while turning */
    float pitch_gain;          /* 0..1 blend per 1/60 second */
    float pitch_down_scale;    /* 0..1 */
    float pitch_limit;         /* 0..128 EQ pitch units */
} HailstoneFollowProfile;

typedef struct HailstoneMountProfile {
    uint32_t size;
    uint32_t instant_acceleration; /* exactly 0 or 1; preserves computed speed limits */
} HailstoneMountProfile;

typedef struct HailstoneFollowStatus {
    uint32_t size;
    uint32_t active, enhanced, target_id;
    uint64_t target_incarnation;
} HailstoneFollowStatus;

typedef int (*HailstoneFollowProfileFn)(HailstoneFollowProfile* out);
typedef int (*HailstoneMountProfileFn)(HailstoneMountProfile* out);

#endif
