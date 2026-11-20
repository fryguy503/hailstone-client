#include "net/market_packets.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "net/packet_io.h"

namespace hailstone::net {

namespace {

void write_guid(ByteWriter& w, const std::string& serial) { w.fixed_string(serial.substr(0, 17), kItemGuidSize); }

AppPacket three_words(Op op, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    ByteWriter w;
    w.le(a);
    w.le(b);
    w.le(c);
    return make_packet(op, std::move(w));
}

void write_trader_buy(ByteWriter& w, const TraderBuy& v) {
    w.le(v.action);
    w.le(v.method);
    w.le(v.sub_action);
    w.le(v.unknown012);
    w.le(v.trader_id);
    w.fixed_string(v.buyer_name.substr(0, 63), 64);
    w.fixed_string(v.seller_name.substr(0, 63), 64);
    w.zeros(32);
    w.fixed_string(v.item_name.substr(0, 63), 64);
    w.fixed_string(v.serial.substr(0, 16), 17);
    w.zeros(3);
    w.le(v.item_id);
    w.le(v.price);
    w.le(v.already_sold);
    w.le(v.unknown276);
    w.le(v.quantity);
}

TraderBuy read_trader_buy(ByteReader& r) {
    TraderBuy v;
    v.action = r.le<std::uint32_t>();
    v.method = r.le<std::uint32_t>();
    v.sub_action = r.le<std::uint32_t>();
    v.unknown012 = r.le<std::uint32_t>();
    v.trader_id = r.le<std::uint32_t>();
    v.buyer_name = r.fixed_string(64);
    v.seller_name = r.fixed_string(64);
    r.skip(32);
    v.item_name = r.fixed_string(64);
    v.serial = r.fixed_string(17);
    r.skip(3);
    v.item_id = r.le<std::uint32_t>();
    v.price = r.le<std::uint32_t>();
    v.already_sold = r.le<std::uint32_t>();
    v.unknown276 = r.le<std::uint32_t>();
    v.quantity = r.le<std::uint32_t>();
    return v;
}

void write_bulk_trader(ByteWriter& w, const BazaarTrader& t) {
    w.le(t.zone_id);
    w.le(t.zone_instance);
    w.le(t.trader_id);
    w.le(t.entity_id);
    detail::write_cstring(w, t.name);
}

} // namespace

// ---------------------------------------------------------------- trader mode

AppPacket encode(const TraderBegin& v) {
    ByteWriter w;
    w.le(kTraderBegin);
    for (const auto& s : v.serials) write_guid(w, s);
    for (auto p : v.prices) w.le(p);
    return make_packet(Op::Trader, std::move(w));
}

std::optional<TraderBegin> decode_trader_begin(const AppPacket& p) {
    if (p.data.size() < kTraderBeginSize) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) {
        TraderBegin v;
        r.skip(4);
        for (auto& s : v.serials) s = r.fixed_string(kItemGuidSize);
        for (auto& x : v.prices) x = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode_trader_action(std::uint32_t action) { return three_words(Op::Trader, action, 0, 0); }

AppPacket encode(const TraderPrice& v) {
    ByteWriter w;
    w.le(v.action);
    write_guid(w, v.serial);
    w.zeros(2);
    w.le(v.count);
    w.le(v.price);
    return make_packet(Op::Trader, std::move(w));
}

std::optional<TraderPrice> decode_trader_price(const AppPacket& p) {
    if (p.data.size() < kTraderPriceSize) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) {
        TraderPrice v;
        v.action = r.le<std::uint32_t>();
        v.serial = r.fixed_string(kItemGuidSize);
        r.skip(2);
        v.count = r.le<std::uint32_t>();
        v.price = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const TraderBuy& v, Op op) {
    ByteWriter w;
    write_trader_buy(w, v);
    return make_packet(op, std::move(w));
}

std::optional<TraderBuy> decode_trader_buy(const AppPacket& p) {
    if (p.data.size() < kTraderBuySize) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) { return read_trader_buy(r); });
}

std::optional<TraderMessage> decode_trader_message(const AppPacket& p) {
    if (p.data.size() < 4) return std::nullopt;
    return detail::decode_with(p, [&](ByteReader& r) {
        TraderMessage m;
        m.action = r.le<std::uint32_t>();
        switch (m.action) {
        case kTraderListItems:
            if (p.data.size() >= kTraderBeginSize) {
                std::vector<std::string> serials(kTraderSlots);
                for (auto& s : serials) s = r.fixed_string(kItemGuidSize);
                for (int i = 0; i < kTraderSlots; ++i)
                    m.listing.emplace_back(std::move(serials[static_cast<std::size_t>(i)]), r.le<std::uint32_t>());
            }
            break;
        case kTraderBuyItem:
            if (p.data.size() >= kTraderBuySize) {
                r.seek(0);
                m.sale = read_trader_buy(r);
                m.word1 = m.sale->method;
                m.word2 = m.sale->sub_action;
            }
            break;
        case kTraderItemMessage:
            if (p.data.size() >= 0x48) {
                m.name = r.fixed_string(64);
                m.code21 = r.le<std::uint32_t>();
            }
            break;
        default:
            if (r.remaining() >= 4) m.word1 = r.le<std::uint32_t>();
            if (r.remaining() >= 4) m.word2 = r.le<std::uint32_t>();
            break;
        }
        return m;
    });
}

AppPacket encode_trader_status(std::uint32_t action, std::uint32_t word1, std::uint32_t word2) {
    return three_words(Op::Trader, action, word1, word2);
}

AppPacket encode_trader_listing(const std::vector<std::pair<std::string, std::uint32_t>>& listing) {
    ByteWriter w;
    w.le(kTraderListItems);
    for (int i = 0; i < kTraderSlots; ++i)
        write_guid(w, static_cast<std::size_t>(i) < listing.size() ? listing[static_cast<std::size_t>(i)].first : std::string());
    for (int i = 0; i < kTraderSlots; ++i)
        w.le(static_cast<std::size_t>(i) < listing.size() ? listing[static_cast<std::size_t>(i)].second : 0u);
    return make_packet(Op::Trader, std::move(w));
}

AppPacket encode(const BecomeTrader& v) {
    ByteWriter w;
    w.le(v.spawn_id);
    w.le(v.trader);
    return make_packet(Op::BecomeTrader, std::move(w));
}

std::optional<BecomeTrader> decode_become_trader(const AppPacket& p) {
    if (p.data.size() < 8) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) {
        BecomeTrader v;
        v.spawn_id = r.le<std::uint32_t>();
        v.trader = r.le<std::uint32_t>();
        return v;
    });
}

// ---------------------------------------------------------------- shopping

AppPacket encode(const TraderClick& v) { return three_words(Op::TraderShop, v.action, v.trader_id, v.approval); }

std::optional<TraderClick> decode_trader_click(const AppPacket& p) {
    if (p.data.size() < 9) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) {
        TraderClick v;
        v.action = r.le<std::uint32_t>();
        v.trader_id = r.le<std::uint32_t>();
        v.approval = r.le<std::uint8_t>(); // the third byte of the trader record
        return v;
    });
}

AppPacket encode_trader_shop_end() {
    ByteWriter w;
    w.le(kTraderEndTransaction);
    return make_packet(Op::TraderShop, std::move(w));
}

AppPacket encode_trader_shop_action(std::uint32_t action) { return three_words(Op::TraderShop, action, 0, 0); }

AppPacket encode_find_market_spawn(std::uint32_t spawn_id) {
    ByteWriter w;
    w.le(spawn_id);
    return AppPacket(kOpFindMarketSpawn, w.take());
}

AppPacket encode(const BazaarQuery& v) {
    ByteWriter w;
    bool scoped = v.trader != 0xffffffffu;
    w.le(kBazaarSearchAction);
    w.le<std::uint8_t>(scoped ? 1 : 0);
    w.zeros(3 + 4 + 4); // +5..+15 (the original leaves +5..+11 uninitialised; +12 is 0)
    w.le(scoped ? v.trader : 0u);
    w.le(v.class_);
    w.le(v.race);
    w.le(v.stat);
    w.le(v.slot);
    w.le(v.type);
    w.fixed_string(v.name.substr(0, 63), 64);
    w.le(v.min_cost);
    w.le(v.max_cost);
    w.le(v.min_level);
    w.le(v.max_level);
    w.le(v.max_results);
    w.le(v.prestige);
    w.le(v.augment);
    return make_packet(Op::TraderShop, std::move(w));
}

std::optional<BazaarQuery> decode_bazaar_query(const AppPacket& p) {
    if (p.data.size() < kBazaarQuerySize) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) {
        BazaarQuery v;
        r.skip(4);
        bool scoped = r.le<std::uint8_t>() != 0;
        r.skip(11);
        v.trader = r.le<std::uint32_t>();
        if (!scoped) v.trader = 0xffffffffu;
        v.class_ = r.le<std::uint32_t>();
        v.race = r.le<std::uint32_t>();
        v.stat = r.le<std::uint32_t>();
        v.slot = r.le<std::uint32_t>();
        v.type = r.le<std::uint32_t>();
        v.name = r.fixed_string(64);
        v.min_cost = r.le<std::uint32_t>();
        v.max_cost = r.le<std::uint32_t>();
        v.min_level = r.le<std::uint32_t>();
        v.max_level = r.le<std::uint32_t>();
        v.max_results = r.le<std::uint32_t>();
        v.prestige = r.le<std::uint32_t>();
        v.augment = r.le<std::uint32_t>();
        return v;
    });
}

AppPacket encode(const BazaarInspect& v) {
    ByteWriter w;
    w.le(kBazaarInspectAction);
    w.le(std::uint32_t{0});
    w.le(v.trader_id);
    write_guid(w, v.serial);
    w.zeros(2);
    w.le(v.item_id);
    w.le(v.flags);
    return make_packet(Op::TraderShop, std::move(w));
}

std::optional<BazaarInspect> decode_bazaar_inspect(const AppPacket& p) {
    if (p.data.size() < kBazaarInspectSize) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) {
        BazaarInspect v;
        r.skip(8);
        v.trader_id = r.le<std::uint32_t>();
        v.serial = r.fixed_string(kItemGuidSize);
        r.skip(2);
        v.item_id = r.le<std::uint32_t>();
        v.flags = r.le<std::uint32_t>();
        return v;
    });
}

std::optional<TraderShopMessage> decode_trader_shop(const AppPacket& p) {
    if (p.data.size() < 4) return std::nullopt;
    return detail::decode_with(p, [&](ByteReader& r) {
        TraderShopMessage m;
        m.action = r.le<std::uint32_t>();
        switch (m.action) {
        case kBazaarWelcome:
            r.skip(4);
            m.traders = r.le<std::uint32_t>();
            m.items = r.le<std::uint32_t>();
            break;
        case kTraderBuyItem:
            r.seek(0);
            m.buy = read_trader_buy(r);
            break;
        case kBazaarAddTrader:
            m.trader.zone_id = r.le<std::uint16_t>();
            m.trader.zone_instance = r.le<std::uint16_t>();
            m.trader.trader_id = r.le<std::uint32_t>();
            m.trader.entity_id = r.le<std::uint32_t>();
            m.trader.name = r.fixed_string(std::min<std::size_t>(64, r.remaining()));
            break;
        case kBazaarRemoveTrader:
            m.trader.trader_id = r.le<std::uint32_t>();
            break;
        case kTraderClick:
            r.seek(0);
            m.click = TraderClick{};
            m.click->action = r.le<std::uint32_t>();
            m.click->trader_id = r.le<std::uint32_t>();
            m.click->approval = r.le<std::uint8_t>();
            break;
        default:
            break;
        }
        return m;
    });
}

AppPacket encode_bazaar_welcome(std::uint32_t traders, std::uint32_t items) {
    ByteWriter w;
    w.le(kBazaarWelcome);
    w.le(std::uint32_t{0});
    w.le(traders);
    w.le(items);
    return make_packet(Op::TraderShop, std::move(w));
}

AppPacket encode_bazaar_add_trader(const BazaarTrader& t) {
    ByteWriter w;
    w.le(kBazaarAddTrader);
    w.le(t.zone_id);
    w.le(t.zone_instance);
    w.le(t.trader_id);
    w.le(t.entity_id);
    w.fixed_string(t.name.substr(0, 63), 64);
    return make_packet(Op::TraderShop, std::move(w));
}

std::optional<std::vector<BazaarTrader>> decode_trader_bulk(const AppPacket& p) {
    return detail::decode_with(p, [](ByteReader& r) {
        std::vector<BazaarTrader> v;
        std::uint32_t n = r.le<std::uint32_t>();
        if (n > 100000) throw std::out_of_range("trader count");
        for (std::uint32_t i = 0; i < n; ++i) {
            BazaarTrader t;
            t.zone_id = r.le<std::uint16_t>();
            t.zone_instance = r.le<std::uint16_t>();
            t.trader_id = r.le<std::uint32_t>();
            t.entity_id = r.le<std::uint32_t>();
            t.name = r.cstring();
            v.push_back(std::move(t));
        }
        return v;
    });
}

AppPacket encode_trader_bulk(const std::vector<BazaarTrader>& v) {
    ByteWriter w;
    w.le(static_cast<std::uint32_t>(v.size()));
    for (const auto& t : v) write_bulk_trader(w, t);
    return make_packet(Op::TraderBulkSend, std::move(w));
}

std::optional<BazaarResults> decode_bazaar_results(const AppPacket& p) {
    return detail::decode_with(p, [](ByteReader& r) {
        BazaarResults v;
        v.action = r.le<std::uint32_t>();
        v.zone_id = r.le<std::uint16_t>();
        v.trader_id = r.le<std::uint32_t>();
        auto count = static_cast<std::int32_t>(r.le<std::uint32_t>());
        for (std::int32_t i = 0; i < count && !r.empty(); ++i) {
            BazaarResult e;
            e.trader_id = r.le<std::uint32_t>();
            e.serial = r.cstring();
            e.cost = r.le<std::uint32_t>();
            e.quantity = r.le<std::uint32_t>();
            e.item_id = r.le<std::uint32_t>();
            e.icon = r.le<std::uint32_t>();
            e.name = r.cstring();
            e.stat = r.le<std::uint32_t>();
            v.results.push_back(std::move(e));
        }
        return v;
    });
}

std::optional<std::uint16_t> decode_bazaar_voucher_cost(const AppPacket& p) {
    return detail::decode_with(p, [](ByteReader& r) {
        r.skip(4);
        return r.le<std::uint16_t>();
    });
}

AppPacket encode(const BazaarResults& v) {
    ByteWriter w;
    w.le(v.action);
    w.le(v.zone_id);
    w.le(v.trader_id);
    w.le(static_cast<std::uint32_t>(v.results.size()));
    for (const auto& e : v.results) {
        w.le(e.trader_id);
        detail::write_cstring(w, e.serial);
        w.le(e.cost);
        w.le(e.quantity);
        w.le(e.item_id);
        w.le(e.icon);
        detail::write_cstring(w, e.name);
        w.le(e.stat);
    }
    return make_packet(Op::BazaarSearch, std::move(w));
}

std::optional<TraderDelItem> decode_trader_del_item(const AppPacket& p) {
    if (p.data.size() < 25) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) {
        TraderDelItem v;
        r.skip(4);
        v.trader_id = r.le<std::uint32_t>();
        v.serial = r.fixed_string(17);
        return v;
    });
}

AppPacket encode(const TraderDelItem& v) {
    ByteWriter w;
    w.le(std::uint32_t{0});
    w.le(v.trader_id);
    w.fixed_string(v.serial.substr(0, 17), 17);
    w.le(std::uint32_t{0});
    return make_packet(Op::TraderDelItem, std::move(w));
}


// ---------------------------------------------------------------- barter

namespace {

// CUnSerializeBuffer as the barter readers use it: a field past the end is skipped (left as it was).
struct LenientReader {
    std::span<const std::byte> data;
    std::size_t pos = 0;

    // Written so that a server-sent length near 2^32 cannot wrap pos + n on a 32-bit size_t.
    bool has(std::size_t n) const { return pos <= data.size() && n <= data.size() - pos; }
    std::uint32_t u32(std::uint32_t fallback = 0) {
        if (!has(4)) return fallback;
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= std::to_integer<std::uint32_t>(data[pos + i]) << (8 * i);
        pos += 4;
        return v;
    }
    std::uint8_t u8(std::uint8_t fallback = 0) {
        if (!has(1)) return fallback;
        return std::to_integer<std::uint8_t>(data[pos++]);
    }
    // CUnSerializeBuffer::GetString(dst, max) / 0x62e430's string: up to max - 1 characters; a string without its
    // terminator in the packet reads as empty and moves nothing.
    // Since pos never moves back, a failed search is remembered: a packet with no NUL in its tail costs one scan,
    // not one per string field read after it.
    std::string str(std::size_t max) {
        if (pos >= no_nul_from || pos >= data.size()) return {};
        const void* nul = std::memchr(data.data() + pos, 0, data.size() - pos);
        if (!nul) {
            no_nul_from = pos;
            return {};
        }
        const std::size_t n = static_cast<std::size_t>(static_cast<const std::byte*>(nul) - (data.data() + pos));
        std::string s(reinterpret_cast<const char*>(data.data() + pos), std::min(n, max - 1));
        pos += n + 1;
        return s;
    }

    std::size_t no_nul_from = std::numeric_limits<std::size_t>::max(); // str()'s memo: no NUL from here on
};

// 0x7d6ef0: a buy line; a line that is not enabled keeps the defaults after its flag.
BuyLine read_line(LenientReader& r) {
    BuyLine v;
    v.slot = r.u32();
    v.enabled = r.u8() != 0;
    if (!v.enabled) return v;
    v.item_id = r.u32();
    v.item_name = r.str(64);
    v.icon = r.u32(v.icon);
    v.quantity = r.u32();
    v.toggle = r.u8() != 0;
    v.cost = r.u32();
    std::uint32_t n = r.u32();
    if (n > kMaxBuyLineTrades) throw std::out_of_range("trade items");
    for (std::uint32_t i = 0; i < n && r.has(12); ++i) {
        BuyLineTrade t;
        t.item_id = r.u32();
        t.quantity = r.u32();
        t.icon = r.u32();
        t.name = r.str(64);
        v.trade_items.push_back(std::move(t));
    }
    v.buyer_spawn_id = r.u32();
    v.buyer_id = r.u32();
    v.zone = r.u32();
    v.buyer_name = r.str(64);
    return v;
}

// A buy line's names are read back into 64-byte fields (0x7d6ef0, GetString(.., 64); EQEmu's
// DECODE(OP_BuyerItems) strcpy's a sold line's item name into char[64]): at most 63 characters.
void write_name(ByteWriter& w, std::string_view s) { detail::write_cstring(w, s.substr(0, 63)); }

// BarterBuyLine_WriteToBuffer @0x7d7060.
void write_line(ByteWriter& w, const BuyLine& v) {
    w.le(v.slot);
    w.le<std::uint8_t>(v.enabled ? 1 : 0);
    if (!v.enabled) return;
    w.le(v.item_id);
    write_name(w, v.item_name);
    w.le(v.icon);
    w.le(v.quantity);
    w.le<std::uint8_t>(v.toggle ? 1 : 0);
    w.le(v.cost);
    w.le(static_cast<std::uint32_t>(v.trade_items.size()));
    for (const auto& t : v.trade_items) {
        w.le(t.item_id);
        w.le(t.quantity);
        w.le(t.icon);
        write_name(w, t.name);
    }
    w.le(v.buyer_spawn_id);
    w.le(v.buyer_id);
    w.le(v.zone);
    write_name(w, v.buyer_name);
}

// EQEmu's buy line (ENCODE(OP_BuyerItems)): every field, without the buyer fields.
void write_emu_line(ByteWriter& w, const BuyLine& v) {
    w.le(v.slot);
    w.le<std::uint8_t>(v.enabled ? 1 : 0);
    w.le(v.item_id);
    write_name(w, v.item_name);
    w.le(v.icon);
    w.le(v.quantity);
    w.le<std::uint8_t>(v.toggle ? 1 : 0);
    w.le(v.cost);
    w.le(static_cast<std::uint32_t>(v.trade_items.size()));
    for (const auto& t : v.trade_items) {
        w.le(t.item_id);
        w.le(t.quantity);
        w.le(t.icon);
        write_name(w, t.name);
    }
}

std::optional<std::uint32_t> first_word(const AppPacket& p) {
    if (p.data.size() < 4) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) { return r.le<std::uint32_t>(); });
}

// The fixed C string at `at` (at most `max` bytes), "" when the packet ends first.
std::string fixed_at(const AppPacket& p, std::size_t at, std::size_t max) {
    if (p.data.size() <= at) return {};
    std::size_t n = 0;
    while (n < max && at + n < p.data.size() && p.data[at + n] != std::byte{0}) ++n;
    return std::string(reinterpret_cast<const char*>(p.data.data() + at), n);
}

} // namespace

std::optional<std::uint32_t> barter_action(const AppPacket& p) { return first_word(p); }

AppPacket encode_barter_inspect(std::uint32_t action, std::uint32_t buyer_id) {
    return three_words(Op::Barter, action, buyer_id, 0);
}

std::optional<BarterInspectReply> decode_barter_inspect_reply(const AppPacket& p) {
    if (first_word(p) != kBuyerInspectBegin || p.data.size() < 12) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) {
        r.skip(4);
        BarterInspectReply v;
        v.spawn_id = r.le<std::uint32_t>();
        v.approval = r.le<std::uint32_t>();
        return v;
    });
}

std::optional<BuyLine> decode_buy_line(const AppPacket& p) {
    auto action = first_word(p);
    if (action != kBuyerSendBuyLine && action != kBuyerModifyBuyLine) return std::nullopt;
    try {
        LenientReader r{p.data, 8};
        return read_line(r);
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
}

AppPacket encode_buy_line(std::uint32_t action, const BuyLine& v) {
    ByteWriter w;
    w.le(action);
    w.le(action == kBuyerSendBuyLine ? v.slot : 0u);
    write_emu_line(w, v);
    return make_packet(Op::BuyerItems, std::move(w));
}

std::optional<BuyerAppearance> decode_buyer_appearance(const AppPacket& p) {
    if (first_word(p) != kBuyerAppearance || p.data.size() < 73) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) {
        r.skip(4);
        BuyerAppearance v;
        v.spawn_id = r.le<std::uint32_t>();
        r.skip(64);
        v.enabled = r.le<std::uint8_t>() != 0;
        return v;
    });
}

AppPacket encode_buyer_appearance(const BuyerAppearance& v) {
    ByteWriter w;
    w.le(kBuyerAppearance);
    w.le(v.spawn_id);
    w.zeros(64);
    w.le<std::uint32_t>(v.enabled ? 1 : 0);
    return make_packet(Op::Barter, std::move(w));
}

AppPacket encode(const BuyerSale& v) {
    // BarterBuyLine_Serialize after the action.
    ByteWriter w;
    w.le(kBuyerSellItem);
    w.le(v.method);
    w.le(v.result);
    w.le(v.buyer_spawn_id);
    w.le(v.buyer_id);
    w.zeros(8);
    detail::write_cstring(w, v.buyer_name);
    detail::write_cstring(w, v.item_name);
    detail::write_cstring(w, v.seller_name);
    write_line(w, v.line);
    w.le(v.quantity);
    w.le(v.count);
    return make_packet(Op::BuyerItems, std::move(w));
}

namespace {

// 0x423500: the sale record from the reader's place.
BuyerSale read_sale(LenientReader& r) {
    BuyerSale v;
    v.method = r.u32();
    v.result = r.u32();
    v.buyer_spawn_id = r.u32();
    v.buyer_id = r.u32();
    r.u32();
    r.u32();
    v.buyer_name = r.str(64);
    v.item_name = r.str(32);
    v.seller_name = r.str(64);
    v.line = read_line(r);
    v.quantity = r.u32();
    v.count = r.u32();
    return v;
}

} // namespace

std::optional<BuyerSale> decode_buyer_sale(const AppPacket& p) {
    if (first_word(p) != kBuyerSellItem) return std::nullopt;
    try {
        LenientReader r{p.data, 4};
        return read_sale(r);
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
}

std::optional<BuyerSaleResult> decode_buyer_sale_result(const AppPacket& p) {
    auto action = first_word(p);
    if (action != kBuyerSellItem && action != kBuyerBuyItem) return std::nullopt;
    try {
        LenientReader r{p.data, 4};
        return BuyerSaleResult{*action, read_sale(r)};
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
}

AppPacket encode_buyer_sale_result(std::uint32_t action, std::uint32_t sub_action, std::uint32_t error,
                                   const std::string& buyer, const std::string& item, const std::string& seller,
                                   std::uint32_t cost, const std::vector<BuyLineTrade>& trade, std::uint32_t quantity) {
    ByteWriter w;
    w.le(action);
    w.le(sub_action);
    w.le(error);
    w.zeros(16);
    detail::write_cstring(w, buyer);
    detail::write_cstring(w, item);
    detail::write_cstring(w, seller);
    w.le<std::uint32_t>(0xffffffff);
    w.le<std::uint32_t>(0xffffffff);
    w.zeros(1);
    detail::write_cstring(w, item);
    w.zeros(9);
    w.le(cost);
    w.le(static_cast<std::uint32_t>(trade.size()));
    for (const auto& t : trade) {
        w.le<std::uint32_t>(0);
        w.le(t.quantity);
        w.le<std::uint32_t>(0);
        detail::write_cstring(w, t.name);
    }
    for (std::uint32_t x : {0u, 0u, 0u, 0xffffffu, 0u}) w.le(x);
    w.le(quantity);
    return make_packet(Op::BuyerItems, std::move(w));
}

AppPacket encode(const BarterSearch& v) {
    ByteWriter w;
    w.le(kBarterSearchAction);
    w.fixed_string(v.text.substr(0, 63), 64);
    w.le(v.transaction_id);
    w.le<std::uint32_t>(0);
    w.le(v.buyer_id);
    w.le<std::uint32_t>(v.local ? 1 : 0);
    return make_packet(Op::Barter, std::move(w));
}

std::optional<BarterSearchResults> decode_barter_search_results(const AppPacket& p) {
    if (first_word(p) != kBarterSearchAction) return std::nullopt;
    try {
        LenientReader r{p.data, 4};
        BarterSearchResults v;
        v.text = r.str(64);
        v.transaction_id = r.u32();
        r.u32();
        r.u32();
        r.u8();
        std::uint32_t n = r.u32();
        if (n > 10000) return std::nullopt;
        // A read past the end gives defaults rather than failing, so a count with nothing behind it would make
        // that many empty lines: stop once the data runs out or a line reads nothing.
        for (std::uint32_t i = 0; i < n && r.has(1); ++i) {
            const std::size_t at = r.pos;
            BuyLine line = read_line(r);
            if (r.pos == at) break;
            v.lines.push_back(std::move(line));
        }
        return v;
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
}

AppPacket encode_barter_search_results(const BarterSearchResults& v) {
    ByteWriter w;
    w.le(kBarterSearchAction);
    detail::write_cstring(w, v.text);
    w.le(v.transaction_id);
    w.le<std::uint32_t>(0);
    w.le<std::uint32_t>(0);
    w.le<std::uint8_t>(1);
    w.le(static_cast<std::uint32_t>(v.lines.size()));
    for (const auto& l : v.lines) write_line(w, l);
    return make_packet(Op::BuyerItems, std::move(w));
}

AppPacket encode_buyer_mode(bool on) {
    ByteWriter w;
    w.le(on ? kBuyerModeOn : kBuyerModeOff);
    return make_packet(Op::Barter, std::move(w));
}

std::optional<bool> decode_buyer_mode(const AppPacket& p) {
    auto a = first_word(p);
    if (a != kBuyerModeOn && a != kBuyerModeOff) return std::nullopt;
    return a == kBuyerModeOn;
}

AppPacket encode_buyer_item_search(const std::string& text) {
    ByteWriter w;
    w.le<std::uint32_t>(kBuyerSearchResults);
    w.fixed_string(text.substr(0, 63), 64);
    return make_packet(Op::Barter, std::move(w));
}

std::optional<std::vector<BuyerItemResult>> decode_buyer_item_results(const AppPacket& p) {
    if (p.data.size() < 8 || first_word(p) != kBuyerSearchResults) return std::nullopt;
    return detail::decode_with(p, [&](ByteReader& r) -> std::vector<BuyerItemResult> {
        r.skip(4);
        std::uint32_t n = r.le<std::uint32_t>();
        if (static_cast<std::uint64_t>(n) * 76 + 8 > p.data.size()) throw std::out_of_range("item result count");
        std::vector<BuyerItemResult> v;
        for (std::uint32_t i = 0; i < n; ++i) {
            BuyerItemResult e;
            e.name = r.fixed_string(64);
            e.item_id = r.le<std::uint32_t>();
            e.icon = r.le<std::uint32_t>();
            r.le<std::uint32_t>();
            v.push_back(std::move(e));
        }
        return v;
    });
}

AppPacket encode_buyer_item_results(const std::vector<BuyerItemResult>& v) {
    ByteWriter w;
    w.le<std::uint32_t>(kBuyerSearchResults);
    w.le(static_cast<std::uint32_t>(v.size()));
    for (const auto& e : v) {
        w.fixed_string(e.name.substr(0, 63), 64);
        w.le(e.item_id);
        w.le(e.icon);
        w.le<std::uint32_t>(0);
    }
    return make_packet(Op::Barter, std::move(w));
}

AppPacket encode_buy_lines(const std::vector<BuyLine>& lines) {
    ByteWriter w;
    w.le(kBuyerBuyLine);
    w.le(static_cast<std::uint16_t>(lines.size()));
    for (const auto& l : lines) write_line(w, l);
    return make_packet(Op::BuyerItems, std::move(w));
}

AppPacket encode_modify_buy_line(const BuyLine& line) {
    ByteWriter w;
    w.le(kBuyerModifyBuyLine);
    write_line(w, line);
    return make_packet(Op::BuyerItems, std::move(w));
}

AppPacket encode_remove_buy_line(std::uint32_t slot) {
    ByteWriter w;
    w.le(kBuyerRemoveItem);
    w.le(slot);
    return make_packet(Op::Barter, std::move(w));
}

std::optional<std::uint32_t> decode_remove_buy_line(const AppPacket& p) {
    if (first_word(p) != kBuyerRemoveItem || p.data.size() < 8) return std::nullopt;
    return detail::decode_with(p, [](ByteReader& r) {
        r.skip(4);
        return r.le<std::uint32_t>();
    });
}

AppPacket encode_buyer_welcome(const std::string& message) {
    ByteWriter w;
    w.le(kBuyerWelcomeMessage);
    w.zeros(68);
    w.fixed_string(message.substr(0, 255), 256);
    return make_packet(Op::Barter, std::move(w));
}

AppPacket encode_barter_action(std::uint32_t action) {
    ByteWriter w;
    w.le(action);
    return make_packet(Op::Barter, std::move(w));
}

AppPacket encode_buyer_greeting(std::uint32_t buyer_id) {
    ByteWriter w;
    w.le(kBuyerGreeting);
    w.le(buyer_id);
    return make_packet(Op::Barter, std::move(w));
}

AppPacket encode(const BarterItemInspect& v) {
    ByteWriter w;
    w.le(kBuyerItemInspect);
    w.le<std::uint32_t>(0);
    w.le(v.buyer);
    w.le(v.zone);
    w.le(v.slot);
    w.le(static_cast<std::uint32_t>(v.index));
    w.le(v.item_id);
    w.le<std::uint32_t>(v.shift ? 2 : 0);
    return make_packet(Op::Barter, std::move(w));
}

AppPacket encode_barter_search_item_inspect(std::uint32_t item_id, bool shift) {
    return three_words(Op::Barter, kBarterItemInspect, item_id, shift ? 2 : 0);
}

std::optional<std::string> decode_buyer_browsing(const AppPacket& p) {
    if (first_word(p) != kBuyerBrowsingBuyLine) return std::nullopt;
    return fixed_at(p, 4, 64);
}

std::optional<BuyerGreeting> decode_buyer_greeting(const AppPacket& p) {
    if (first_word(p) != kBuyerGreeting) return std::nullopt;
    return BuyerGreeting{fixed_at(p, 8, 64), fixed_at(p, 0x48, 1024)};
}

namespace {

// 0x6368c0: {u32 zone, u32 character id, u32 spawn id, name}.
BarterBuyer read_buyer(LenientReader& r) {
    BarterBuyer b;
    b.zone = r.u32();
    b.buyer_id = r.u32();
    b.spawn_id = r.u32();
    b.name = r.str(64);
    return b;
}

} // namespace

std::optional<BarterBuyer> decode_barter_add_buyer(const AppPacket& p) {
    if (first_word(p) != kBarterAddBuyer || p.data.size() < 16) return std::nullopt;
    BarterBuyer b;
    LenientReader r{p.data, 4};
    b.zone = r.u32();
    b.buyer_id = r.u32();
    b.spawn_id = r.u32();
    b.name = fixed_at(p, 16, 64);
    return b;
}

std::optional<std::uint32_t> decode_barter_remove_buyer(const AppPacket& p) {
    if (first_word(p) != kBarterRemoveBuyer || p.data.size() < 8) return std::nullopt;
    LenientReader r{p.data, 4};
    return r.u32();
}

std::optional<std::vector<BarterBuyer>> decode_barter_buyer_list(const AppPacket& p) {
    if (first_word(p) != kBarterBuyerList) return std::nullopt;
    LenientReader r{p.data, 4};
    std::uint32_t n = r.u32();
    if (n > 10000) return std::nullopt;
    std::vector<BarterBuyer> v;
    for (std::uint32_t i = 0; i < n && r.has(1); ++i) { // as the search results: no empty buyers past the data
        const std::size_t at = r.pos;
        BarterBuyer b = read_buyer(r);
        if (r.pos == at) break;
        v.push_back(std::move(b));
    }
    return v;
}

std::optional<std::string> decode_buy_lines_text(const AppPacket& p) {
    if (first_word(p) != kBuyerLinesText) return std::nullopt;
    // SerializeBuffer_ReadCXStr: a u32 length, then the text.
    LenientReader r{p.data, 4};
    std::uint32_t n = r.u32();
    if (!r.has(n)) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(p.data.data() + r.pos), n);
}

} // namespace hailstone::net
