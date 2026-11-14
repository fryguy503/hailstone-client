#include "game/augments.h"

#include <algorithm>
#include <format>
#include <initializer_list>

#include "game/eq_strings.h"
#include "game/item_preview.h"
#include "game/merchant.h"
#include "game/mechanics/item_stats.h"

// Each function names the original it follows; the fit checks are checked against the original's vectors.

namespace hailstone::game {

namespace m = mechanics;

namespace {

std::string str(const EqStrings* s, std::uint32_t id, std::vector<std::string> args = {}) {
    return s ? s->format(id, args) : std::string();
}

// The 64-bit serial (+0xf8): the header's instance_id and parcel_item_id.
bool same_serial(const net::Item& a, const net::Item& b) {
    return a.instance_id == b.instance_id && a.parcel_item_id == b.parcel_item_id;
}

constexpr std::uint64_t host_types(std::initializer_list<std::uint32_t> types) {
    std::uint64_t mask = 0;
    for (std::uint32_t t : types) mask |= std::uint64_t{1} << t;
    return mask;
}

// ItemTypes: 0 1H slashing, 1 2H slashing, 2 piercing, 3 1H blunt, 4 2H blunt, 8 shield, 0xa armour, 0x23 2H
// piercing, 0x2d hand to hand.
constexpr std::uint64_t kOneHanded = host_types({0, 2, 3, 0x2d});
constexpr std::uint64_t kTwoHanded = host_types({1, 4, 0x23});
// Restriction n (1..18) at [n - 1].
constexpr std::array<std::uint64_t, 18> kHostTypesByRestriction{
    host_types({8, 0xa}),                            // 1
    kOneHanded | kTwoHanded | host_types({5, 6, 7}), // 2
    kOneHanded,                                      // 3
    kTwoHanded,                                      // 4
    host_types({0}),                                 // 5
    host_types({3}),                                 // 6
    host_types({2}),                                 // 7
    host_types({0x2d}),                              // 8
    host_types({1}),                                 // 9
    host_types({4}),                                 // 10
    host_types({0x23}),                              // 11
    host_types({5}),                                 // 12
    host_types({8}),                                 // 13
    host_types({0, 3, 0x2d}),                        // 14
    host_types({3, 0xd}),                            // 15
    host_types({0, 1}),                              // 16
    host_types({3, 4}),                              // 17
    host_types({2, 0x23}),                           // 18
};

// The augment restriction's allowed host ItemTypes (+0x1d4); 0 and values above 0x12 allow all
// (ItemBase_CanAugmentGoInItem's switch).
bool type_allowed(std::uint32_t restriction, std::uint32_t t) {
    if (restriction == 0 || restriction > kHostTypesByRestriction.size()) return true;
    if (t >= 64) return false;
    return (kHostTypesByRestriction[restriction - 1] >> t & 1u) != 0;
}

// ItemContainer_FindItemByID @0x7e5280 / 0x7e4f90 over the host's children: a depth-first search for `id` (0
// matches the first item), the path written at index[level] (when below 3) and reset below each searched child.
// The path starts as the host's own index (its children's container is at depth = the host's index length).
bool find_by_id(const net::Item& item, std::uint32_t id, int level, std::array<std::int16_t, 3>& path) {
    std::vector<const net::BagItem*> children;
    for (const auto& c : item.contents) children.push_back(&c);
    std::sort(children.begin(), children.end(), [](auto* a, auto* b) { return a->index < b->index; });
    for (const net::BagItem* c : children) {
        if (level < 3) path[static_cast<std::size_t>(level)] = static_cast<std::int16_t>(c->index);
        if (c->item.id == id || id == 0) return true;
        if (!c->item.contents.empty()) {
            bool found = find_by_id(c->item, id, level + 1, path);
            if (found) return true;
            if (level + 1 < 3) path[static_cast<std::size_t>(level + 1)] = -1;
        }
    }
    return false;
}

// ItemGlobalIndex_IsWornSlot @0x4322b0.
bool is_worn(const net::ItemSlot& s) {
    return s.type == net::kInvPossessions && s.slot != -1 && s.sub == -1 && s.slot <= 0x16;
}

net::AugmentItem block_for(const net::Item& host, int socket, net::AugmentAction action) {
    net::AugmentItem b;
    b.serial = host.instance_id;
    b.serial_high = host.parcel_item_id;
    b.host = host.slot;
    b.socket = socket;
    b.action = action;
    return b;
}

AugmentRequest refuse(std::uint32_t eqstr) {
    AugmentRequest r;
    r.kind = AugmentRequest::Refuse;
    r.eqstr = eqstr;
    return r;
}

AugmentRequest send(net::AugmentItem block) {
    AugmentRequest r;
    r.kind = AugmentRequest::Send;
    r.packet = block;
    return r;
}

AugmentRequest confirm(std::uint32_t dialog, std::uint32_t eqstr, std::vector<std::string> args, net::AugmentItem block) {
    AugmentRequest r;
    r.kind = AugmentRequest::Confirm;
    r.dialog = dialog;
    r.eqstr = eqstr;
    r.args = std::move(args);
    r.packet = block;
    return r;
}

// The start every request shares: nothing for an invalid or hidden socket or without a
// PC; the host by its index (when the serial matches), else by its serial; 5473 when neither finds it.
struct Start {
    bool done = false;
    AugmentRequest result;
    const net::Item* host = nullptr;
};
Start start(const net::Item& window_item, int socket, const AugmentContext& ctx) {
    Start s;
    if (!aug_socket_valid(window_item, socket) || !aug_socket_visible(window_item, socket) || !ctx.has_pc) {
        s.done = true;
        return s;
    }
    const net::Item* host = ctx.item_at ? ctx.item_at(window_item.slot) : nullptr;
    if (!host || !same_serial(*host, window_item))
        host = ctx.item_by_serial ? ctx.item_by_serial(window_item.instance_id, window_item.parcel_item_id) : nullptr;
    if (!host) {
        s.done = true;
        s.result = refuse(5473);
        return s;
    }
    s.host = host;
    return s;
}

// The checks insert and swap share after the fit test: prestige (0xc → 7686), the appearance
// feature for a type-21 socket (0xd → 5485), and the NO TRADE rule (3364). 0 passes.
std::uint32_t usability(const net::Item& host, const net::Item& cursor, int socket, const AugmentContext& ctx) {
    if (ctx.can_use_prestige && !ctx.can_use_prestige(cursor)) return augment_fit_string_id(0xc);
    if (is_special_ornament_socket_type(aug_socket_type(host, socket)) && !ctx.appearance_feature)
        return augment_fit_string_id(0xd);
    // @006a83db: either item not droppable (ItemClient::CanDrop(0, 1, 0, 1), vtable +0x1c) or an Attuneable augment
    // needs both usable by the player's class.
    if (!is_droppable(host) || !is_droppable(cursor) || cursor.attuneable) {
        bool ok = !ctx.can_use || (ctx.can_use(host) && ctx.can_use(cursor));
        if (!ok) return 3364;
    }
    return 0;
}

// A distiller for the augment's SolventItemID: the item with that id, else (and on
// remove also instead of a type-55 match) the first Perfected Augmentation Distiller.
std::optional<FoundItem> find_distiller(std::uint32_t solvent, bool discard_solvent, const AugmentContext& ctx) {
    std::optional<FoundItem> d = ctx.item_by_id ? ctx.item_by_id(solvent) : std::nullopt;
    if (d && (!d->item || (discard_solvent && d->item->item_type == kItemTypeSolvent))) d.reset();
    if (!d && ctx.item_by_type) d = ctx.item_by_type(kItemTypePerfectedDistiller);
    if (d && !d->item) d.reset();
    return d;
}

// An augment that is sent at once: no solvent, and not Attuneable or already attuned (NoDropFlag +0x80).
bool sends_at_once(const net::Item& aug) { return !has_solvent(aug) && (!aug.attuneable || aug.attuned != 0); }

} // namespace

// ---------------------------------------------------------------- sockets

bool aug_socket_valid(const net::Item& item, int socket) {
    return socket >= 0 && socket < kAugSockets && item.aug_sockets[static_cast<std::size_t>(socket)].type != 0;
}

bool aug_socket_visible(const net::Item& item, int socket) {
    return socket >= 0 && socket < kAugSockets && item.aug_sockets[static_cast<std::size_t>(socket)].visible != 0;
}

std::uint32_t aug_socket_type(const net::Item& item, int socket) {
    return socket >= 0 && socket < kAugSockets ? item.aug_sockets[static_cast<std::size_t>(socket)].type : 0;
}

const net::Item* augment_in(const net::Item& item, int socket) {
    if (socket < 0) return nullptr;
    return item.content(static_cast<std::uint32_t>(socket));
}

WornLook worn_look(const net::Item& item, int material_slot) {
    WornLook w;
    w.tint = item.color;
    if (material_slot >= 7) {
        const std::string& ornament =
            material_slot == 8 && !item.ornament_off.empty() ? item.ornament_off : item.ornament_main;
        w.material = item_model_number(ornament);
        if (w.material == 0) w.material = item_model_number(item.id_file);
        return w;
    }
    w.material = item.material;
    w.hero_forge = item.ornament_hero_model != 0 ? item.ornament_hero_model : item.heros_forge_model;
    for (int socket = kAugSockets - 1; socket >= 0; --socket) {
        if (!is_ornament_socket_type(aug_socket_type(item, socket))) continue;
        const net::Item* aug = augment_in(item, socket);
        if (!aug) continue;
        if (!aug->aug_texture_disabled && aug->material != 0) w.material = aug->material;
        break;
    }
    return w;
}

void update_stats_disabled(net::Item& item, std::int32_t use_item, std::int32_t ornament, bool top_level) {
    const std::int32_t prestige = item.item_class != 0 ? 0 : static_cast<std::int32_t>(item.prestige);
    item.rank_disabled = use_item != -1 && use_item < prestige;
    if (item.item_class == 1) {
        for (auto& c : item.contents) {
            update_stats_disabled(c.item, use_item, ornament, false);
            c.item.aug_texture_disabled = false;
        }
    } else if (item.item_class == 0) {
        for (int socket = 0; socket < kAugSockets; ++socket) {
            net::Item* aug = item.content(static_cast<std::uint32_t>(socket));
            if (!aug) continue;
            update_stats_disabled(*aug, use_item, ornament, false);
            aug->aug_texture_disabled = ornament == 0 && aug_socket_type(item, socket) == 21;
        }
    }
    if (top_level) item.aug_texture_disabled = false;
}

bool is_augmented(const net::Item& item) {
    for (int i = 0; i < kAugSockets; ++i)
        if (aug_socket_valid(item, i) && aug_socket_visible(item, i) && augment_in(item, i)) return true;
    return false;
}

// ---------------------------------------------------------------- the fit test

bool is_weapon_type(const net::Item& item) {
    std::uint8_t t = item.item_type;
    if (t == 0x1b || t == 0x1c || t == 0x2b || t == 0x2c) return false;
    return static_cast<std::int32_t>(item.damage) > 0;
}

bool can_augment_go_in_item(const net::Item& host, const net::Item& aug) {
    bool fits = type_allowed(aug.aug_restrict, host.item_type);
    if (is_weapon_type(host)) {
        int delay = aug.delay;
        if (delay > 0 && host.delay < delay) return false;
    }
    // The host's subclass (+0x5a4) must be in the augment's socket subclass list when it has one (+0x609 / +0x60c;
    // EQEmu's RoF2 items carry none).
    if (fits && !aug.socket_subclasses.empty() &&
        std::find(aug.socket_subclasses.begin(), aug.socket_subclasses.end(), host.sub_type) == aug.socket_subclasses.end())
        return false;
    return fits;
}

int check_augment_combined_restrictions(const net::Item& host, const net::Item* aug, std::uint32_t worn_classes) {
    std::uint32_t host_classes = 0;
    if (aug && worn_classes != 0 && is_worn(host.slot)) {
        if (aug->classes != 0 && (aug->classes & worn_classes) == 0) return 2;
        net::Item with = host; // the host's own Classes with the player's, its children ANDed in as before
        with.classes |= worn_classes;
        host_classes = m::item_classes(with);
    } else {
        host_classes = m::item_classes(host);
    }
    if (!aug || aug->aug_type == 0) return 0xf;
    std::uint32_t host_deity = m::item_deity(host);
    std::uint32_t races = m::item_races(*aug) & m::item_races(host);
    std::uint32_t classes = m::item_classes(*aug) & host_classes;
    std::uint32_t aug_deity = m::item_deity(*aug);
    std::uint32_t deity = aug_deity & host_deity;
    std::uint32_t slots = m::item_equip_slots(*aug) & m::item_equip_slots(host);
    if (aug->no_transfer != host.no_transfer) return 0xb;
    if (races == 0) return 1;
    if (classes == 0) return 2;
    if (deity == 0 && host_deity != 0 && aug_deity != 0) return 3;
    if (slots == 0) return 4;
    if (!can_augment_go_in_item(host, *aug)) return 9;
    return host.max_stack > 1 ? 0xe : 0; // EQ_Item::IsStackable @0x7b0380
}

int can_gem_fit_in_slot(const net::Item& host, const net::Item* aug, std::uint32_t socket, bool check_empty,
                        bool check_dup, std::uint32_t worn_classes) {
    if (int r = check_augment_combined_restrictions(host, aug, worn_classes)) return r;
    if (socket > 5) return 6;
    std::uint32_t type = host.aug_sockets[socket].type; // +0x1ec + 8 * socket
    if (type == 0) return 6;
    if ((aug->aug_type & (1u << ((type - 1) & 31))) == 0) return 7;
    if ((static_cast<std::uint32_t>(aug->aug_restrict2) & (1u << (host.material & 31))) == 0) return 8;
    if (check_empty && augment_in(host, static_cast<int>(socket))) return 5;
    if (check_dup) {
        // ItemContainer_FindItemByID on the host's children, then ItemIndex_GetLastSlot @0x7afdb0 of the path.
        std::array<std::int16_t, 3> path{-1, -1, -1};
        const std::int16_t own[3] = {host.slot.slot, host.slot.sub, host.slot.aug};
        int depth = 0;
        while (depth < 3 && own[depth] != -1) {
            path[static_cast<std::size_t>(depth)] = own[depth];
            ++depth;
        }
        if (depth < 3) path[static_cast<std::size_t>(depth)] = 0;
        if (find_by_id(host, aug->id, depth, path)) {
            std::int32_t last = -1;
            for (int i = 2; i >= 0; --i)
                if (path[static_cast<std::size_t>(i)] != -1) {
                    last = path[static_cast<std::size_t>(i)];
                    break;
                }
            if (last != static_cast<std::int32_t>(socket)) return 10;
        }
    }
    if (is_worn(host.slot)) {
        auto slot = static_cast<std::uint16_t>(host.slot.slot);
        if (((1u << (slot & 31)) & m::item_equip_slots(*aug)) == 0) return 0x10;
    }
    return 0;
}

std::uint32_t augment_fit_string_id(int result) {
    // The eqstr id for each fit result 0..0x10 (0x7afe40); any other result says 3362.
    static constexpr std::array<std::uint32_t, 0x11> kFitStrings{
        0,    3365, 3365, 3365, 3365, 3366, 5476, 3367, 5480,
        5480, 3371, 6193, 7686, 5485, 5477, 5475, 5365,
    };
    if (result < 0 || result >= static_cast<int>(kFitStrings.size())) return 3362;
    return kFitStrings[static_cast<std::size_t>(result)];
}

std::uint32_t aug_restriction_string_id(std::uint32_t restriction) {
    static constexpr std::uint32_t kIds[16] = {0,    9134, 9135, 9136, 9137, 9138, 9139, 9140,
                                               9148, 9141, 9142, 9143, 9144, 9145, 8052, 9200};
    return restriction < 16 ? kIds[restriction] : 0;
}

// ---------------------------------------------------------------- the item display

std::string augment_socket_row_text(const net::Item& item, int socket, const AugmentStrings& s, bool highlight) {
    const std::uint32_t type = aug_socket_type(item, socket);
    // CDBStr::GetString(type, 16): "1 (Stats)", ...; the number when missing.
    std::string type_name = s.db_string ? s.db_string(type, 16) : std::string();
    if (type_name.empty()) type_name = std::to_string(type);
    const net::Item* aug = augment_in(item, socket);
    std::string third;
    if (!aug) {
        third = str(s.strings, 6699); // empty
    } else {
        // CStmlWnd::MakeWndNotificationTag(27, name, "0^i") @0087e130.
        third = std::format("<a WndNotify=\"27,0^{}\">{}</a>", socket, aug->name);
    }
    std::string row = str(s.strings, 3355, {std::to_string(socket + 1), type_name, third});
    auto wrap = [](std::uint32_t argb, const std::string& text) {
        return std::format("<C \"#{:06X}\">{}</C>", argb & 0xffffff, text); // CStmlWnd::MakeStmlColorTag @0087e0d0
    };
    if (highlight) return wrap(kAugFitArgb, row);
    if (aug && stats_disabled(*aug)) return wrap(kAugDisabledArgb, row);
    return row;
}

AugmentSockets build_augment_sockets(const net::Item& item, const net::Item* cursor, const AugmentStrings& s,
                                     bool appearance_feature, std::uint32_t worn_classes) {
    AugmentSockets out;
    for (int i = 0; i < kAugSockets; ++i) {
        if (!aug_socket_valid(item, i) || !aug_socket_visible(item, i)) continue;
        const net::Item* aug = augment_in(item, i);
        const std::uint32_t type = aug_socket_type(item, i);
        // The highlight: occupied sockets included, no duplicate check.
        bool fits =
            cursor && can_gem_fit_in_slot(item, cursor, static_cast<std::uint32_t>(i), false, false, worn_classes) == 0;
        AugmentSocketRow row;
        row.text = augment_socket_row_text(item, i, s, fits);
        row.filled = aug != nullptr;
        row.icon = aug ? aug->icon : 0; // IconCache::GetIcon(ItemBase::GetImageNum(aug))
        if (!is_ornament_socket_type(type)) {
            row.shown = true;
            row.tooltip = str(s.strings, aug ? 4671 : 4670);
            out.rows[static_cast<std::size_t>(i)] = std::move(row);
        } else {
            // The appearance area instead (the later ornament socket wins); the row-visible flag stays 0.
            out.appearance_shown = true;
            out.appearance_socket = i;
            out.buy_shown = !aug && is_special_ornament_socket_type(type) && !appearance_feature;
            row.shown = true;
            row.tooltip = str(s.strings, aug ? 4671 : 4673);
            out.appearance = std::move(row);
        }
    }
    out.buy_tooltip = out.buy_shown ? str(s.strings, 4672) : std::string();
    return out;
}

std::string augment_description_rows(const net::Item& item, const AugmentStrings& s) {
    std::string out;
    for (int i = 0; i < kAugSockets; ++i)
        if (aug_socket_valid(item, i) && aug_socket_visible(item, i))
            out += augment_socket_row_text(item, i, s, false) + "<BR>";
    return out;
}

std::string augment_slot_types_line(const net::Item& aug, const AugmentStrings& s) {
    std::string list;
    int bits = 0;
    for (std::uint32_t t = 1; t <= 31; ++t) {
        if ((aug.aug_type & (1u << (t - 1))) == 0) continue; // ItemBase_GetAugType @007b04d0
        std::string name = s.db_string ? s.db_string(t, 16) : std::string();
        if (name.empty()) name = std::to_string(t);
        list += (bits == 0 ? " " : ", ") + name;
        ++bits;
    }
    return str(s.strings, 3357, {bits >= 31 ? str(s.strings, 3369) : list});
}

std::string augment_restrictions_block(const net::Item& aug, const AugmentStrings& s) {
    std::string out = "<BR>" + str(s.strings, 3358) + " -<BR>";
    if (std::uint32_t id = aug_restriction_string_id(aug.aug_restrict)) out += str(s.strings, id) + "<BR>";
    if (aug.delay > 0) out += str(s.strings, 9147, {std::to_string(aug.delay)}) + "<BR>"; // ItemBase_GetDelay
    return out;
}

std::string augment_effects_restriction(const net::Item& aug, const AugmentStrings& s) {
    // CItemDisplayWnd_CreateEffectsString: "<BR>", 3358, " -<BR>", the restriction, "<BR>".
    std::uint32_t id = aug.item_type == kItemTypeAugment ? aug_restriction_string_id(aug.aug_restrict) : 0;
    if (id == 0) return {};
    return "<BR>" + str(s.strings, 3358) + " -<BR>" + str(s.strings, id) + "<BR>";
}

// ---------------------------------------------------------------- requests

AugmentRequest insert_augment_request(const net::Item& window_item, int socket, const AugmentContext& ctx) {
    Start s = start(window_item, socket, ctx);
    if (s.done) return s.result;
    const net::Item& host = *s.host;
    if (augment_in(host, socket)) return refuse(3366);            // @006a82e9
    const net::Item* cursor = ctx.cursor;
    if (!cursor) return refuse(5479);                             // @006a8322
    if (cursor->stack_count > 1) return refuse(5484);             // @006a833e
    if (int r = can_gem_fit_in_slot(host, cursor, static_cast<std::uint32_t>(socket), true, true, ctx.worn_classes))
        return refuse(augment_fit_string_id(r));
    if (std::uint32_t id = usability(host, *cursor, socket, ctx)) return refuse(id);
    net::AugmentItem block = block_for(host, socket, net::AugmentAction::Insert);
    if (sends_at_once(*cursor)) return send(block);              // @006a847c
    return confirm(kConfirmAugInsert, 5880, {cursor->name, host.name}, block); // @006a8560
}

AugmentRequest remove_augment_request(const net::Item& window_item, int socket, const AugmentContext& ctx) {
    Start s = start(window_item, socket, ctx);
    if (s.done) return s.result;
    const net::Item& host = *s.host;
    const net::Item* aug = augment_in(host, socket);
    if (!aug) return refuse(3363);
    if (ctx.cursor) return refuse(5478);
    net::AugmentItem block = block_for(host, socket, net::AugmentAction::Remove);
    if (!has_solvent(*aug)) return send(block); // @006a8a50
    std::optional<FoundItem> d = find_distiller(aug->aug_distiller, true, ctx); // @006a886d, type test @006a88ac
    if (!d) return refuse(5474);
    block.distiller = d->slot;
    return confirm(kConfirmAugRemove, 5879, {d->item->name, aug->name, host.name}, block); // @006a8928
}

AugmentRequest swap_augment_request(const net::Item& window_item, int socket, const AugmentContext& ctx) {
    Start s = start(window_item, socket, ctx);
    if (s.done) return s.result;
    const net::Item& host = *s.host;
    const net::Item* old = augment_in(host, socket);
    if (!old) return refuse(3363);
    const net::Item* cursor = ctx.cursor;
    if (!cursor) return refuse(5479);
    if (cursor->stack_count > 1) return refuse(5484);
    if (int r = can_gem_fit_in_slot(host, cursor, static_cast<std::uint32_t>(socket), false, true, ctx.worn_classes))
        return refuse(augment_fit_string_id(r));
    if (std::uint32_t id = usability(host, *cursor, socket, ctx)) return refuse(id);
    net::AugmentItem block = block_for(host, socket, net::AugmentAction::Swap);
    if (has_solvent(*old)) { // @006a8e44: a type-55 match is kept here
        std::optional<FoundItem> d = find_distiller(old->aug_distiller, false, ctx);
        if (!d) return refuse(5474);
        block.distiller = d->slot;
        return confirm(kConfirmAugSwap, 5481, {d->item->name, old->name, cursor->name, host.name}, block); // @006a8e55
    }
    if (sends_at_once(*cursor)) return send(block); // @006a9057
    return confirm(kConfirmAugSwap, 5482, {old->name, cursor->name, host.name}, block); // @006a9141
}

AugmentRequest destroy_augment_request(const net::Item& window_item, int socket, const AugmentContext& ctx) {
    Start s = start(window_item, socket, ctx);
    if (s.done) return s.result;
    const net::Item& host = *s.host;
    const net::Item* aug = augment_in(host, socket);
    if (!aug) return {};
    return confirm(kConfirmAugDestroy, 5878, {aug->name, host.name},
                   block_for(host, socket, net::AugmentAction::Destroy));
}

AugmentRequest augment_socket_click(const net::Item& window_item, int socket, const AugmentContext& ctx) {
    // sub_006ae010 reads the window's item (its child, the cursor).
    if (!augment_in(window_item, socket)) return insert_augment_request(window_item, socket, ctx);
    if (!ctx.cursor) return remove_augment_request(window_item, socket, ctx);
    return swap_augment_request(window_item, socket, ctx);
}

std::vector<AugmentMenuRow> augment_socket_menu(const net::Item& window_item, int socket, const AugmentContext& ctx,
                                                const EqStrings* strings) {
    std::vector<AugmentMenuRow> rows;
    if (window_item.slot.type != net::kInvPossessions || !ctx.has_pc) return rows;
    const net::Item* host = ctx.item_at ? ctx.item_at(window_item.slot) : nullptr;
    if (!host || !same_serial(*host, window_item))
        host = ctx.item_by_serial ? ctx.item_by_serial(window_item.instance_id, window_item.parcel_item_id) : nullptr;
    if (!host) return rows;
    const net::Item* aug = augment_in(*host, socket);
    if (!aug) return rows;
    rows.push_back({aug->name, 1, 0xff00ff00, false});
    if (!ctx.cursor) rows.push_back({str(strings, 5470), kAugMenuRemove, 0xff8aa3ff, true});
    else rows.push_back({str(strings, 5483), kAugMenuSwap, 0xff8aa3ff, true});
    if (has_solvent(*aug)) rows.push_back({str(strings, 5471), kAugMenuDestroy, 0xffc08080, true});
    return rows;
}

} // namespace hailstone::game
