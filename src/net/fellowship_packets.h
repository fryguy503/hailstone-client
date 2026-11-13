#pragma once

// Native RoF2 fellowship messages. The roster ceiling of 64 is a deliberate
// extension; membership and gameplay limits remain the server's decision.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/packet.h"

namespace hailstone::net {

inline constexpr std::size_t kMaxFellowshipMembers = 64;
inline constexpr std::size_t kNativeFellowshipMembers = 12;
inline constexpr std::uint16_t kOpFellowshipRequest = 0x7121;
inline constexpr std::uint16_t kOpFellowshipInvite = 0x62ab;
inline constexpr std::uint16_t kOpFellowshipRoster = 0x6e4d;
inline constexpr std::uint16_t kOpCampsiteKitRequest = 0x0c12;
inline constexpr std::uint16_t kOpCampsiteKitList = 0x074d;
inline constexpr std::uint16_t kOpFellowshipVitality = 0x2cc6;

enum class FellowshipAction : std::uint32_t {
    Create = 1, Motd = 2, Accept = 3, Invite = 5, Remove = 6,
    Promote = 7, CreateCamp = 8, DestroyCamp = 9, Leave = 10,
    End = 11, ToggleSharing = 12
};

struct FellowshipMember {
    std::uint32_t character_id = 0;
    std::string name;
    std::uint32_t zone_id = 0, level = 0, class_ = 0, last_seen = 0;
    bool sharing = false, capped = false, offline = false;
};
struct FellowshipCamp {
    float x = 0, y = 0, z = 0;
    std::uint32_t zone_id = 0, expires = 0;
    bool active = false;
};
struct FellowshipKit {
    bool available = false;
    std::uint32_t camp_type_id = 0, description_id = 0;
    std::string name;
};
struct FellowshipInvite {
    std::uint32_t inviter_id = 0;
    std::string inviter_name;
};
struct FellowshipVitality {
    std::uint64_t normal = 0, normal_cap = 0;
    std::uint32_t aa = 0, aa_cap = 0;
};
struct FellowshipState {
    std::uint32_t id = 0;
    std::string name, motd;
    std::vector<FellowshipMember> members;
    FellowshipCamp camp;
    std::vector<FellowshipKit> kits;
    std::optional<FellowshipInvite> invite;
    FellowshipVitality vitality;

    bool active() const { return id != 0 && !members.empty(); }
    const FellowshipMember* member(std::uint32_t character_id) const;
    const FellowshipMember* member(std::string_view name) const;
    bool is_leader(std::string_view name) const;
};

AppPacket encode_fellowship_request(FellowshipAction action, std::uint32_t target = 0,
                                    std::string_view motd = {}, std::uint32_t camp_type = 0);
AppPacket encode_fellowship_kit_request(bool available_only, std::uint32_t category_mask);
std::optional<FellowshipState> decode_fellowship_roster(const AppPacket& p);
std::optional<FellowshipInvite> decode_fellowship_invite(const AppPacket& p);
std::optional<std::vector<FellowshipKit>> decode_fellowship_kits(const AppPacket& p);
std::optional<FellowshipVitality> decode_fellowship_vitality(const AppPacket& p);

} // namespace hailstone::net
