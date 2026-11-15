#include "net/evolving_packets.h"

#include "net/packet_io.h"

namespace hailstone::net {

AppPacket encode(const EvolvingRequest& value) {
    ByteWriter w;
    w.le(static_cast<std::uint32_t>(value.action));
    w.le(std::uint32_t{0});
    w.le(value.source_id);
    w.le(value.destination_id);
    return make_packet(Op::EvolveItem, std::move(w));
}

AppPacket encode(const EvolvingUpdate& value) {
    ByteWriter w;
    w.le(static_cast<std::uint32_t>(value.action));
    w.le(std::uint32_t{0});
    w.le(value.id);
    w.le(value.percentage);
    w.le(value.activated);
    return make_packet(Op::EvolveItem, std::move(w));
}

AppPacket encode(const EvolvingTransferDetails& value) {
    ByteWriter w;
    w.le(static_cast<std::uint32_t>(EvolvingAction::Details));
    w.le(value.source_id);
    w.le(value.destination_id);
    w.le(value.compatibility);
    w.le(value.max_transfer_level);
    w.le(static_cast<std::uint8_t>(value.source ? 1 : 0));
    w.le(static_cast<std::uint8_t>(value.destination ? 1 : 0));
    if (value.source) write_item(w, *value.source);
    if (value.destination) write_item(w, *value.destination);
    return make_packet(Op::EvolveItem, std::move(w));
}

AppPacket encode(const EvolvingFinalResult& value) {
    // CItemDisplayWnd::Send_OP_EvolveItem @0x69ef60: source template, final-form hint, current level,
    // the bare-item link hash, and !Shift. This is not the persistent-ID layout used by transfer.
    ByteWriter w;
    w.le(static_cast<std::uint32_t>(EvolvingAction::FinalResult));
    w.le(value.item_id);
    w.le(value.final_item_id);
    w.le(value.level);
    w.le(value.hash);
    w.le(static_cast<std::uint32_t>(value.full ? 1 : 0));
    return make_packet(Op::EvolveItem, std::move(w));
}

std::optional<EvolvingRequest> decode_evolving_request(const AppPacket& p) {
    if (p.data.size() != kEvolvingRequestSize) return std::nullopt;
    auto value = detail::decode_with(p, [](ByteReader& r) {
        EvolvingRequest v;
        v.action = static_cast<EvolvingAction>(r.le<std::uint32_t>());
        r.skip(4);
        v.source_id = r.le<std::uint64_t>();
        v.destination_id = r.le<std::uint64_t>();
        return v;
    });
    if (value && value->action != EvolvingAction::Details && value->action != EvolvingAction::Transfer)
        return std::nullopt;
    return value;
}

std::optional<EvolvingUpdate> decode_evolving_update(const AppPacket& p) {
    if (p.data.size() != kEvolvingRequestSize) return std::nullopt;
    auto value = detail::decode_with(p, [](ByteReader& r) {
        EvolvingUpdate v;
        v.action = static_cast<EvolvingAction>(r.le<std::uint32_t>());
        r.skip(4);
        v.id = r.le<std::uint64_t>();
        v.percentage = r.le<std::uint32_t>();
        v.activated = r.le<std::uint32_t>();
        return v;
    });
    if (value && value->action != EvolvingAction::Update && value->action != EvolvingAction::Open &&
        value->action != EvolvingAction::FinalResult) return std::nullopt;
    return value;
}

std::optional<EvolvingTransferDetails> decode_evolving_details(const AppPacket& p) {
    if (p.data.size() < kEvolvingDetailsHeaderSize) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) {
        if (r.le<std::uint32_t>() != static_cast<std::uint32_t>(EvolvingAction::Details))
            throw std::out_of_range("evolving action");
        EvolvingTransferDetails v;
        v.source_id = r.le<std::uint64_t>();
        v.destination_id = r.le<std::uint64_t>();
        v.compatibility = r.le<std::uint32_t>();
        v.max_transfer_level = r.le<std::uint32_t>();
        const auto source = r.le<std::uint8_t>();
        const auto destination = r.le<std::uint8_t>();
        if (source > 1 || destination > 1) throw std::out_of_range("evolving item presence");
        if (source) v.source = read_item(r);
        if (destination) v.destination = read_item(r);
        if (!r.empty()) throw std::out_of_range("evolving trailing data");
        return v;
    });
}

} // namespace hailstone::net
