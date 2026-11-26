#include "net/marketplace_packets.h"

#include <cstring>
#include <span>

#include "core/byte_io.h"
#include "net/packet_io.h"

namespace hailstone::net {

namespace {

// CUnSerializeBuffer {buf, len, cursor} as the marketplace readers use it: each read checks cursor + size <= len
// and leaves its target alone when it does not fit (ReadInt32 @0x4048b0, the inline copies of it). The cursor may
// pass the end (a capped string skips its whole length); every later read then fails.
class LenientReader {
public:
    explicit LenientReader(std::span<const std::byte> data) : data_(data) {}

    bool fits(std::size_t n) const { return pos_ <= data_.size() && n <= data_.size() - pos_; }
    bool exhausted() const { return !fits(1); }
    std::size_t position() const { return pos_; }

    template <typename T> bool read(T& out) {
        if (!fits(sizeof(T))) return false;
        ByteReader r(data_.subspan(pos_, sizeof(T)));
        out = r.le<T>();
        pos_ += sizeof(T);
        return true;
    }

    // MarketplaceItem_Deserialize's strings: L = strlen + 1, n = min(L, cap); when n bytes fit they are copied and
    // cut at n, and the cursor moves by L; otherwise the string is empty and the cursor stays. The original's strlen
    // runs past the packet when there is no NUL; here the packet's end ends the string.
    void cstring(std::string& out, std::size_t cap) {
        const std::size_t left = pos_ <= data_.size() ? data_.size() - pos_ : 0;
        const char* p = left ? reinterpret_cast<const char*>(data_.data() + pos_) : nullptr;
        const void* nul = left ? std::memchr(p, 0, left) : nullptr;
        const std::size_t len = nul ? static_cast<std::size_t>(static_cast<const char*>(nul) - p) : left;
        const std::size_t total = len + 1, n = total < cap ? total : cap;
        if (n > left) {
            out.clear();
            return;
        }
        out.assign(p, n < len ? n : len);
        advance(total);
    }

    // Stream_ReadString @0x80c9b0: a signed u32 length, then that many bytes; 0 or less is empty. A missing length
    // reads as 0 here (the original takes whatever its argument slot holds); a length past the end takes what is left.
    void lpstring(std::string& out) {
        std::int32_t n = 0;
        read(n);
        if (n <= 0) {
            out.clear();
            return;
        }
        const std::size_t left = pos_ <= data_.size() ? data_.size() - pos_ : 0;
        const std::size_t take = static_cast<std::size_t>(n) < left ? static_cast<std::size_t>(n) : left;
        if (take) out.assign(reinterpret_cast<const char*>(data_.data() + pos_), take);
        else out.clear();
        advance(static_cast<std::size_t>(n));
    }

private:
    // A cursor moved past the end stays at the end: data_.data() + pos_ must never point beyond the packet.
    void advance(std::size_t n) { pos_ = n < data_.size() - pos_ ? pos_ + n : data_.size(); }

    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
};

// The absolute time of a relative-seconds field (+0x2100 / +0x2104): 0 never (-1), else now + value. `raw` is what
// the read left, which is `now` itself when the packet had no room for it.
std::int64_t until(std::uint32_t raw, std::int64_t now) {
    return raw == 0 ? -1 : now + static_cast<std::int64_t>(raw);
}

std::uint32_t relative(std::int64_t t, std::int64_t now) {
    if (t < 0) return 0;
    return static_cast<std::uint32_t>(t - now);
}

void write_capped(ByteWriter& w, const std::string& s, std::size_t cap) {
    detail::write_cstring(w, s.size() < cap ? std::string_view(s) : std::string_view(s).substr(0, cap - 1));
}

// A string at a fixed offset of a raw struct, up to the next NUL, the given length or the packet's end.
std::string fixed_text(const AppPacket& p, std::size_t at, std::size_t max) {
    std::string s;
    for (std::size_t i = at; i < p.data.size() && i < at + max && p.data[i] != std::byte{0}; ++i)
        s.push_back(static_cast<char>(p.data[i]));
    return s;
}

template <typename T> T fixed(const AppPacket& p, std::size_t at) {
    if (at > p.data.size() || sizeof(T) > p.data.size() - at) return T{};
    ByteReader r(std::span<const std::byte>(p.data).subspan(at, sizeof(T)));
    return r.le<T>();
}

template <typename T> void put(std::vector<std::byte>& b, std::size_t at, T v) {
    ByteWriter w;
    w.le(v);
    std::memcpy(b.data() + at, w.data().data(), sizeof(T));
}

} // namespace

// ---------------------------------------------------------------- the catalog

std::optional<std::vector<MarketplaceEntry>> decode_marketplace_items(const AppPacket& p, std::int64_t now) {
    LenientReader r(p.data);
    std::uint32_t count = 0;
    // MarketplaceItemList_Parse @0x7f90e0: a missing count leaves its argument slot (the reader's pointer) as the
    // count; refused here.
    if (!r.read(count)) return std::nullopt;
    // Hailstone's limit, not confirmed in the original (docs/unknowns.md): a catalog counted past kMaxEntries is
    // refused, so a packet of near-empty entries cannot grow into several times its size in memory. A store lists
    // hundreds of items; ten thousand leaves room for a large one.
    constexpr std::uint32_t kMaxEntries = 10000;
    if (count > kMaxEntries) return std::nullopt;
    std::vector<MarketplaceEntry> out;
    for (std::uint32_t i = 0; i < count && !r.exhausted(); ++i) {
        const std::size_t at = r.position();
        MarketplaceEntry e;
        r.read(e.id);
        r.read(e.price);
        r.read(e.icon);
        r.read(e.max_quantity);
        r.cstring(e.name, 0x7f);
        r.cstring(e.category, 0x3ff);
        r.cstring(e.description, 0x3ff);
        r.cstring(e.text14, 0x3f);
        r.read(e.flag8e0);
        std::uint8_t b = 0;
        if (r.read(b)) e.is_new = b != 0;
        r.cstring(e.text8e2, 0x3ff);
        r.cstring(e.purchase_message, 0x3ff);
        if (r.read(b)) e.item_of_week = b != 0;
        r.read(e.item_of_week_rank);
        r.read(e.kind);
        r.read(e.u10ec);
        r.cstring(e.text10f0, 0x3ff);
        r.cstring(e.text14f0, 0x3ff);
        r.cstring(e.text18f0, 0x3ff);
        r.cstring(e.text1cf0, 0x3ff);
        r.read(e.u20f0);
        r.read(e.u20f4);
        if (r.read(b)) e.is_featured = b != 0;
        r.read(e.was_price);
        // time(&t) writes into the variable the next two reads (and +0x2108's) fall back to.
        const auto now32 = static_cast<std::uint32_t>(now);
        std::uint32_t t = now32;
        r.read(t);
        e.available_until = until(t, now);
        t = now32;
        r.read(t);
        e.sale_until = until(t, now);
        e.u2108 = now32;
        r.read(e.u2108);
        r.read(e.money_type);
        r.read(e.u2110);
        r.read(e.u2114);
        out.push_back(std::move(e));
        if (r.position() == at) break; // read nothing: every further entry would be this one again
    }
    return out;
}

AppPacket encode_marketplace_items(const std::vector<MarketplaceEntry>& v, std::int64_t now) {
    ByteWriter w;
    w.le(static_cast<std::uint32_t>(v.size()));
    for (const auto& e : v) {
        w.le(e.id);
        w.le(e.price);
        w.le(e.icon);
        w.le(e.max_quantity);
        write_capped(w, e.name, 0x80);
        write_capped(w, e.category, 0x400);
        write_capped(w, e.description, 0x400);
        write_capped(w, e.text14, 0x40);
        w.le(e.flag8e0);
        w.le(static_cast<std::uint8_t>(e.is_new));
        write_capped(w, e.text8e2, 0x400);
        write_capped(w, e.purchase_message, 0x400);
        w.le(static_cast<std::uint8_t>(e.item_of_week));
        w.le(e.item_of_week_rank);
        w.le(e.kind);
        w.le(e.u10ec);
        write_capped(w, e.text10f0, 0x400);
        write_capped(w, e.text14f0, 0x400);
        write_capped(w, e.text18f0, 0x400);
        write_capped(w, e.text1cf0, 0x400);
        w.le(e.u20f0);
        w.le(e.u20f4);
        w.le(static_cast<std::uint8_t>(e.is_featured));
        w.le(e.was_price);
        w.le(relative(e.available_until, now));
        w.le(relative(e.sale_until, now));
        w.le(e.u2108);
        w.le(e.money_type);
        w.le(e.u2110);
        w.le(e.u2114);
    }
    return AppPacket(kOpMarketplaceItems, w.take());
}

// ---------------------------------------------------------------- funding tiers

std::optional<std::vector<MarketplaceFund>> decode_marketplace_funding(const AppPacket& p) {
    LenientReader r(p.data);
    std::uint32_t count = 0;
    if (!r.read(count)) return std::nullopt; // MarketplaceFundList_Parse @0x7f92b0: as the catalog's
    std::vector<MarketplaceFund> out;
    for (std::uint32_t i = 0; i < count && !r.exhausted(); ++i) {
        const std::size_t at = r.position();
        MarketplaceFund f;
        r.read(f.amount);
        r.read(f.u0c);
        r.cstring(f.text14, 0x3f);
        r.cstring(f.text54, 0x3f);
        out.push_back(std::move(f));
        if (r.position() == at) break;
    }
    return out;
}

AppPacket encode_marketplace_funding(const std::vector<MarketplaceFund>& v) {
    ByteWriter w;
    w.le(static_cast<std::uint32_t>(v.size()));
    for (const auto& f : v) {
        w.le(f.amount);
        w.le(f.u0c);
        write_capped(w, f.text14, 0x40);
        write_capped(w, f.text54, 0x40);
    }
    return AppPacket(kOpMarketplaceFunding, w.take());
}

// ---------------------------------------------------------------- row states

std::optional<std::vector<MarketplaceRowState>> decode_marketplace_states(const AppPacket& p) {
    LenientReader r(p.data);
    std::uint32_t count = 0;
    r.read(count); // a missing count is 0 (the local starts at 0)
    std::vector<MarketplaceRowState> out;
    // Past the end every further entry repeats the last one, which the receiver ignores (it changes only what
    // differs), so the loop stops there.
    MarketplaceRowState s;
    for (std::uint32_t i = 0; i < count && !r.exhausted(); ++i) {
        const std::size_t at = r.position();
        r.read(s.entry_id);
        r.read(s.state);
        r.read(s.item_id);
        r.read(s.u10);
        r.lpstring(s.category);
        out.push_back(s);
        if (r.position() == at) break;
    }
    return out;
}

AppPacket encode_marketplace_states(const std::vector<MarketplaceRowState>& v) {
    ByteWriter w;
    w.le(static_cast<std::uint32_t>(v.size()));
    for (const auto& s : v) {
        w.le(s.entry_id);
        w.le(s.state);
        w.le(s.item_id);
        w.le(s.u10);
        w.le(static_cast<std::uint32_t>(s.category.size()));
        w.bytes({reinterpret_cast<const std::byte*>(s.category.data()), s.category.size()});
    }
    return AppPacket(kOpMarketplaceStates, w.take());
}

// ---------------------------------------------------------------- the wallet

std::optional<MarketplaceWallet> decode_marketplace_wallet(const AppPacket& p) {
    if (p.data.size() < kMarketplaceWalletSize) return std::nullopt;
    MarketplaceWallet v;
    v.status = fixed<std::uint32_t>(p, 0x44);
    v.balance = fixed<std::uint32_t>(p, 0x48);
    v.zip_on_file = fixed<std::uint8_t>(p, 0x4c) != 0;
    v.type = fixed<std::uint32_t>(p, 0x50);
    return v;
}

AppPacket encode(const MarketplaceWallet& v) {
    std::vector<std::byte> b(kMarketplaceWalletSize);
    put(b, 0x44, v.status);
    put(b, 0x48, v.balance);
    put(b, 0x4c, static_cast<std::uint8_t>(v.zip_on_file));
    put(b, 0x50, v.type);
    return AppPacket(kOpMarketplaceWallet, std::move(b));
}

// ---------------------------------------------------------------- purchases

AppPacket encode_marketplace_purchase(std::uint32_t entry_id, std::uint32_t quantity) {
    std::vector<std::byte> b(kMarketplacePurchaseSize);
    put(b, 0x48, entry_id);
    put(b, 0x4c, quantity);
    return AppPacket(kOpMarketplacePurchase, std::move(b));
}

std::optional<MarketplacePurchaseResult> decode_marketplace_purchase_result(const AppPacket& p) {
    MarketplacePurchaseResult v;
    v.entry_id = fixed<std::uint32_t>(p, 0x40);
    v.success = fixed<std::uint8_t>(p, 0xc8) != 0;
    v.station_cash = fixed<std::uint32_t>(p, 0xd8) != 0;
    v.balance = fixed<std::uint32_t>(p, 0xdc);
    v.placed = fixed<std::uint32_t>(p, 0xe0);
    v.code = fixed<std::uint32_t>(p, 0xec);
    v.note_f0 = fixed_text(p, 0xf0, 0x400);
    v.note_4f0 = fixed_text(p, 0x4f0, 0x400);
    return v;
}

AppPacket encode(const MarketplacePurchaseResult& v) {
    std::vector<std::byte> b(kMarketplacePurchaseResultSize);
    put(b, 0x40, v.entry_id);
    put(b, 0xc8, static_cast<std::uint8_t>(v.success));
    put(b, 0xd8, static_cast<std::uint32_t>(v.station_cash));
    put(b, 0xdc, v.balance);
    put(b, 0xe0, v.placed);
    put(b, 0xec, v.code);
    std::memcpy(b.data() + 0xf0, v.note_f0.data(), v.note_f0.size() < 0x3ff ? v.note_f0.size() : 0x3ff);
    std::memcpy(b.data() + 0x4f0, v.note_4f0.data(), v.note_4f0.size() < 0x3ff ? v.note_4f0.size() : 0x3ff);
    return AppPacket(kOpMarketplacePurchaseResult, std::move(b));
}

// ---------------------------------------------------------------- the small requests

AppPacket encode_marketplace_request(std::uint16_t opcode) { return AppPacket(opcode); }

AppPacket encode_marketplace_hello() {
    std::vector<std::byte> b(kMarketplaceHelloSize);
    put(b, 0x48, std::uint32_t{1});
    return AppPacket(kOpMarketplaceHello, std::move(b));
}

AppPacket encode_marketplace_preview_request(std::uint32_t item_id) {
    std::vector<std::byte> b(80);
    put(b, 0x00, item_id);
    put(b, 0x40, std::int32_t{-1});
    put(b, 0x4c, std::uint16_t{1});
    return AppPacket(to_wire(Op::ItemPreviewRequest), std::move(b));
}

} // namespace hailstone::net
