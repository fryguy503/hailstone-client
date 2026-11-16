#ifndef HAILSTONE_RAID_H
#define HAILSTONE_RAID_H

#include <stdint.h>

/* A copied view of the received raid roster, never a request for remote vitals.
 * A revision belongs to one host/plugin instance and changes with membership,
 * subgroup/role changes, the local character or an actor incarnation. */
typedef struct HailstoneRaidRoster {
    uint32_t size;
    uint64_t revision;
    uint64_t owner_incarnation;
    uint32_t count; /* 0..72 */
} HailstoneRaidRoster;

#define HAILSTONE_RAID_LEADER 1u
#define HAILSTONE_RAID_GROUP_LEADER 2u
#define HAILSTONE_RAID_ASSIST 4u
#define HAILSTONE_RAID_MARKER 8u

typedef struct HailstoneRaidMember {
    uint32_t size;
    char name[64];
    uint32_t group_id; /* 0 ungrouped, 1..12 subgroup */
    uint32_t class_id;
    uint32_t level;
    uint32_t spawn_id; /* 0 when no current in-zone actor is known */
    uint64_t incarnation;
    uint32_t flags;
} HailstoneRaidMember;

#define HAILSTONE_RAID_MOVE 1u
#define HAILSTONE_RAID_PROMOTE_GROUP_LEADER 2u

#endif
