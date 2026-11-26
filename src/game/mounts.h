#pragma once

// Riding: how a rider and its mount are linked and unlinked (EQPlayer::GetMount +0x154 / PlayerBase_GetRider
// +0x158 as Spawn::mount_id / rider_id), as the original does it (do_stand @0x593f20, the dismount @0x590510 (vtable +0x60, misnamed SlideIceFriction),
// PlayerClient::ClearVehicleMountLinks @0x5a0400, EQPlayer::do_change_form's re-seat @0x597110..0x5974d2).
//
// There is no mount packet: a mount is an NPC named "<rider>`s_Mount00" of a mount race (EQEmu's Horse, summoned by
// SPA 113), and each client links the two by name when either spawn's actor is made (MakeRiderMountUp: EQPlayer's
// constructor, CEverQuest::CreateInitialActors, do_change_form) or on the buff tick (ProcessAffects' SPA 113 case).
// The local player that mounts a mount race hands its controls to the mount (pinstControlledPlayer,
// WorldState::controlled_id): the mount is moved, and the rider sits on it.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::game {

struct Spawn;
struct WorldState;

// The mount's name for a rider: "%s`s_Mount00" (0x9d77d8).
std::string mount_name_for(std::string_view rider);
// MakeRiderMountUp's rider name for a mount NPC: a name of at least 9 characters holding "`s_Mount" (0x9d77e8) loses
// its last 10 characters (StrNCpyTerminated(name, own, length - 9)); nullopt otherwise. A name of exactly 9
// characters gives an empty name (the original writes its 0 before the buffer then; nothing is found either way).
std::optional<std::string> rider_name_for_mount(std::string_view mount);

// 0x58dde0: the rider's model cannot ride: its race (vf88) is not 367, 522 or 1, it has an actor and the actor
// definition has no tunic point (+0x34). PlayerClient_IsHumanActorDef @0x58dd50: a mount race drawn as the
// HUM / HUF fallback.
bool riding_blocked(const Spawn& s);
bool human_actor_mount(const Spawn& s);

// What the link asks of the client.
class MountQueries {
public:
    virtual ~MountQueries() = default;
    // PlayerClient_AttachRiderToMount's dag 0xe on the mount's model (SetBone +0x19c, FindBoneByName: "<rider
    // code>_MOUNT01" exactly, else "<mount code>RIDER_DAG" as a prefix; the first name it sets,
    // "CHEST03_MOUNT01", is replaced by the second before the check): whether the mount's model has it.
    virtual bool mount_point(const Spawn& rider, const Spawn& mount) = 0;
    // The local player only (do_stand's room test): PlayerZoneClient::CanIFitHere at its position (+0x64, 0) and
    // the room between the ceiling it found (+0x11b4) and the cached floor (+0x1070).
    virtual bool can_i_fit_here() = 0;
    virtual double room_above() = 0;
    // The local player's FloorHeight (+0x28) and the ceiling CanIFitHere last found room under (+0x11b4), for
    // CastSpell's SPA 113 room test; false when neither is known (no zone collision yet).
    virtual bool ceiling_and_floor(float& ceiling, float& floor) {
        (void)ceiling;
        (void)floor;
        return false;
    }
};

// What the caller does after a link change, in this order.
struct MountEvents {
    // token_string_to_client(id, 0xd) lines for the local player: 4067 (0xfe3) no room to mount here, 5524
    // (0x1594) the mount cannot be ridden.
    std::vector<std::uint32_t> messages;
    // GetPCSpellAffect(113) then RemovePCAffectex: the summon mount effect goes.
    bool remove_mount_affect = false;
    // The local player took control of its mount (pinstControlledPlayer = the mount; the player's SpeedRun and
    // SpeedHeading 0, its physics block copied onto the mount, its ViewHeight x 0.85) or gave it back.
    bool took_control = false;
    bool released_control = false;
    // The dismount left the local player's spell book (CSpellBookWnd Deactivate).
    bool close_spell_book = false;
};

// do_stand @0x593f20 (PlayerClient vtable +0xcc): `rider` mounts `mount` once both actors are ready.
void do_stand(WorldState& w, Spawn& rider, Spawn* mount, MountQueries& q, MountEvents& ev);
// The dismount @0x590510 (vtable +0x60): `rider` gets off its mount.
void dismount(WorldState& w, Spawn& rider, MountEvents& ev);
// PlayerZoneClient::MakeRiderMountUp @0x59f310: a mount NPC finds its rider by name, a player its mount.
void make_rider_mount_up(WorldState& w, Spawn& s, MountQueries& q, MountEvents& ev);
// ~PlayerClient @0x590af5: before `id` goes, ClearVehicleMountLinks unseats whoever rides it or whom it rides, and
// no spawn keeps it as its rider.
void clear_vehicle_mount_links(WorldState& w, std::uint32_t id, MountEvents& ev);
// do_change_form's first half @0x597110: a mounted spawn dismounts (its mount kept), else a "<name>`s_Mount" spawn
// (GetPlayerFromPartialName) that has it as rider forgets it. Returns the mount to re-seat afterwards (0 none).
std::uint32_t change_form_unseat(WorldState& w, Spawn& s, MountEvents& ev);
// ProcessAffects' SPA 113 case @0x43cc48, every buff tick: a local player without a mount mounts the spawn
// GetPlayerFromPartialName finds for "<name>`s_Mount" (its name cut to 0x18 characters).
void summon_mount_tick(WorldState& w, MountQueries& q, MountEvents& ev);

// The spawn the local player moves (pinstControlledPlayer): its mount, or itself (null without one).
Spawn* controlled_spawn(WorldState& w);
const Spawn* controlled_spawn(const WorldState& w);

} // namespace hailstone::game
