#include "web/browser_protocol.h"

#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "../../sdk/hailstone_byte_io.h"

namespace hailstone::web {

// The type byte is the message's index in the variant plus one: the order of Message is the wire's.
static_assert(std::variant_size_v<Message> < 255);

namespace {

using hailstone::ByteReader;
using hailstone::ByteWriter;

void str(ByteWriter& w, const std::string& s) {
    w.le(static_cast<std::uint32_t>(s.size()));
    w.bytes({reinterpret_cast<const std::byte*>(s.data()), s.size()});
}
std::string str(ByteReader& r) {
    auto n = r.le<std::uint32_t>();
    auto b = r.bytes(n);
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}
void flag(ByteWriter& w, bool b) { w.le(static_cast<std::uint8_t>(b ? 1 : 0)); }
bool flag(ByteReader& r) {
    auto v = r.le<std::uint8_t>();
    if (v > 1) throw std::out_of_range("flag");
    return v == 1;
}
// An enum's byte, nothing past `last`.
template <typename E> E pick(ByteReader& r, E last) {
    auto v = r.le<std::uint8_t>();
    if (v > static_cast<std::uint8_t>(last)) throw std::out_of_range("enum");
    return static_cast<E>(v);
}
template <typename E> void put(ByteWriter& w, E e) { w.le(static_cast<std::uint8_t>(e)); }

void write(ByteWriter& w, const Hello& m) { w.le(m.version); }
void write(ByteWriter& w, const Open& m) {
    w.le(m.tab);
    w.le(m.width);
    w.le(m.height);
    put(w, m.rules);
    w.le(static_cast<std::uint32_t>(m.allow.size()));
    for (const auto& a : m.allow) str(w, a);
    str(w, m.profile);
    str(w, m.url);
}
void write(ByteWriter& w, const Navigate& m) { w.le(m.tab); str(w, m.url); }
void write(ByteWriter& w, const Resize& m) { w.le(m.tab); w.le(m.width); w.le(m.height); }
void write(ByteWriter& w, const Mouse& m) {
    w.le(m.tab);
    put(w, m.kind);
    put(w, m.button);
    w.le(m.clicks);
    w.le(m.x);
    w.le(m.y);
    w.le(m.mods);
}
void write(ByteWriter& w, const Wheel& m) { w.le(m.tab); w.le(m.x); w.le(m.y); w.le(m.dx); w.le(m.dy); w.le(m.mods); }
void write(ByteWriter& w, const Key& m) { w.le(m.tab); put(w, m.kind); w.le(m.code); w.le(m.character); w.le(m.mods); }
void write(ByteWriter& w, const Text& m) { w.le(m.tab); str(w, m.text); }
void write(ByteWriter& w, const Focus& m) { w.le(m.tab); flag(w, m.on); }
void write(ByteWriter& w, const Nav& m) { w.le(m.tab); put(w, m.action); }
void write(ByteWriter& w, const Eval& m) { w.le(m.tab); str(w, m.script); }
void write(ByteWriter& w, const Post& m) { w.le(m.tab); str(w, m.message); }
void write(ByteWriter& w, const Close& m) { w.le(m.tab); }
void write(ByteWriter&, const Shutdown&) {}
void write(ByteWriter& w, const Ready& m) { w.le(m.version); }
void write(ByteWriter& w, const Frame& m) {
    w.le(m.tab);
    put(w, m.layer);
    w.le(m.width);
    w.le(m.height);
    w.le(static_cast<std::uint32_t>(m.rects.size()));
    for (const auto& r : m.rects) {
        w.le(r.x);
        w.le(r.y);
        w.le(r.width);
        w.le(r.height);
        w.bytes(r.bgra);
    }
}
void write(ByteWriter& w, const Popup& m) {
    w.le(m.tab);
    flag(w, m.shown);
    w.le(m.x);
    w.le(m.y);
    w.le(m.width);
    w.le(m.height);
}
void write(ByteWriter& w, const Address& m) { w.le(m.tab); str(w, m.url); }
void write(ByteWriter& w, const Title& m) { w.le(m.tab); str(w, m.title); }
void write(ByteWriter& w, const Loading& m) {
    w.le(m.tab);
    flag(w, m.loading);
    flag(w, m.can_back);
    flag(w, m.can_forward);
}
void write(ByteWriter& w, const CursorShape& m) { w.le(m.tab); put(w, m.cursor); }
void write(ByteWriter& w, const Console& m) { w.le(m.tab); w.le(m.level); str(w, m.text); }
void write(ByteWriter& w, const PageMessage& m) { w.le(m.tab); str(w, m.message); }
void write(ByteWriter& w, const Closed& m) { w.le(m.tab); put(w, m.reason); str(w, m.detail); }

template <typename T> T read(ByteReader& r);
template <> Hello read<Hello>(ByteReader& r) { return {r.le<std::uint32_t>()}; }
template <> Open read<Open>(ByteReader& r) {
    Open m;
    m.tab = r.le<std::uint32_t>();
    m.width = r.le<std::uint16_t>();
    m.height = r.le<std::uint16_t>();
    m.rules = pick(r, TabRules::Sites);
    auto n = r.le<std::uint32_t>();
    if (n > r.remaining() / 4) throw std::out_of_range("allow count");
    for (std::uint32_t i = 0; i < n; ++i) m.allow.push_back(str(r));
    m.profile = str(r);
    m.url = str(r);
    return m;
}
template <> Navigate read<Navigate>(ByteReader& r) {
    Navigate m;
    m.tab = r.le<std::uint32_t>();
    m.url = str(r);
    return m;
}
template <> Resize read<Resize>(ByteReader& r) {
    Resize m;
    m.tab = r.le<std::uint32_t>();
    m.width = r.le<std::uint16_t>();
    m.height = r.le<std::uint16_t>();
    return m;
}
template <> Mouse read<Mouse>(ByteReader& r) {
    Mouse m;
    m.tab = r.le<std::uint32_t>();
    m.kind = pick(r, MouseKind::Leave);
    m.button = pick(r, MouseButton::Right);
    m.clicks = r.le<std::uint8_t>();
    m.x = r.le<std::int32_t>();
    m.y = r.le<std::int32_t>();
    m.mods = r.le<std::uint32_t>();
    return m;
}
template <> Wheel read<Wheel>(ByteReader& r) {
    Wheel m;
    m.tab = r.le<std::uint32_t>();
    m.x = r.le<std::int32_t>();
    m.y = r.le<std::int32_t>();
    m.dx = r.le<std::int32_t>();
    m.dy = r.le<std::int32_t>();
    m.mods = r.le<std::uint32_t>();
    return m;
}
template <> Key read<Key>(ByteReader& r) {
    Key m;
    m.tab = r.le<std::uint32_t>();
    m.kind = pick(r, KeyKind::Char);
    m.code = r.le<std::uint32_t>();
    m.character = r.le<std::uint32_t>();
    m.mods = r.le<std::uint32_t>();
    return m;
}
template <> Text read<Text>(ByteReader& r) {
    Text m;
    m.tab = r.le<std::uint32_t>();
    m.text = str(r);
    return m;
}
template <> Focus read<Focus>(ByteReader& r) {
    Focus m;
    m.tab = r.le<std::uint32_t>();
    m.on = flag(r);
    return m;
}
template <> Nav read<Nav>(ByteReader& r) {
    Nav m;
    m.tab = r.le<std::uint32_t>();
    m.action = pick(r, NavAction::Stop);
    return m;
}
template <> Eval read<Eval>(ByteReader& r) {
    Eval m;
    m.tab = r.le<std::uint32_t>();
    m.script = str(r);
    return m;
}
template <> Post read<Post>(ByteReader& r) {
    Post m;
    m.tab = r.le<std::uint32_t>();
    m.message = str(r);
    return m;
}
template <> Close read<Close>(ByteReader& r) { return {r.le<std::uint32_t>()}; }
template <> Shutdown read<Shutdown>(ByteReader&) { return {}; }
template <> Ready read<Ready>(ByteReader& r) { return {r.le<std::uint32_t>()}; }
template <> Frame read<Frame>(ByteReader& r) {
    Frame m;
    m.tab = r.le<std::uint32_t>();
    m.layer = pick(r, Layer::Popup);
    m.width = r.le<std::uint16_t>();
    m.height = r.le<std::uint16_t>();
    auto n = r.le<std::uint32_t>();
    if (n > r.remaining() / 8) throw std::out_of_range("rect count");
    m.rects.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        FrameRect fr;
        fr.x = r.le<std::uint16_t>();
        fr.y = r.le<std::uint16_t>();
        fr.width = r.le<std::uint16_t>();
        fr.height = r.le<std::uint16_t>();
        if (std::uint32_t{fr.x} + fr.width > m.width || std::uint32_t{fr.y} + fr.height > m.height)
            throw std::out_of_range("rect outside the frame");
        auto px = r.bytes(std::size_t{fr.width} * fr.height * 4);
        fr.bgra.assign(px.begin(), px.end());
        m.rects.push_back(std::move(fr));
    }
    return m;
}
template <> Popup read<Popup>(ByteReader& r) {
    Popup m;
    m.tab = r.le<std::uint32_t>();
    m.shown = flag(r);
    m.x = r.le<std::int32_t>();
    m.y = r.le<std::int32_t>();
    m.width = r.le<std::uint16_t>();
    m.height = r.le<std::uint16_t>();
    return m;
}
template <> Address read<Address>(ByteReader& r) {
    Address m;
    m.tab = r.le<std::uint32_t>();
    m.url = str(r);
    return m;
}
template <> Title read<Title>(ByteReader& r) {
    Title m;
    m.tab = r.le<std::uint32_t>();
    m.title = str(r);
    return m;
}
template <> Loading read<Loading>(ByteReader& r) {
    Loading m;
    m.tab = r.le<std::uint32_t>();
    m.loading = flag(r);
    m.can_back = flag(r);
    m.can_forward = flag(r);
    return m;
}
template <> CursorShape read<CursorShape>(ByteReader& r) {
    CursorShape m;
    m.tab = r.le<std::uint32_t>();
    m.cursor = pick(r, Cursor::NotAllowed);
    return m;
}
template <> Console read<Console>(ByteReader& r) {
    Console m;
    m.tab = r.le<std::uint32_t>();
    m.level = r.le<std::uint8_t>();
    m.text = str(r);
    return m;
}
template <> PageMessage read<PageMessage>(ByteReader& r) {
    PageMessage m;
    m.tab = r.le<std::uint32_t>();
    m.message = str(r);
    return m;
}
template <> Closed read<Closed>(ByteReader& r) {
    Closed m;
    m.tab = r.le<std::uint32_t>();
    m.reason = pick(r, CloseReason::Failed);
    m.detail = str(r);
    return m;
}

template <std::size_t I> std::optional<Message> read_index(std::size_t index, ByteReader& r) {
    if constexpr (I == std::variant_size_v<Message>) {
        return std::nullopt;
    } else {
        if (index == I) return Message(std::in_place_index<I>, read<std::variant_alternative_t<I, Message>>(r));
        return read_index<I + 1>(index, r);
    }
}

template <typename T, std::size_t I = 0> constexpr std::size_t index_of() {
    if constexpr (std::is_same_v<std::variant_alternative_t<I, Message>, T>) return I;
    else return index_of<T, I + 1>();
}

} // namespace

std::size_t max_message_size(std::uint8_t type) {
    // The type byte, the tab, (the level,) the string's length, the text.
    if (type == index_of<Console>() + 1) return 1 + 4 + 1 + 4 + kMaxConsoleText;
    if (type == index_of<PageMessage>() + 1) return 1 + 4 + 4 + kMaxPageMessage;
    return kMaxMessage;
}

namespace {
// A Frame's body before its rectangles: the type, tab, layer, width, height and count; then 8 bytes and the pixels
// of each rectangle.
constexpr std::size_t kFrameHead = 1 + 4 + 1 + 2 + 2 + 4;
constexpr std::size_t kRectHead = 8;
std::size_t rect_size(const FrameRect& r) { return kRectHead + r.bgra.size(); }
} // namespace

std::size_t frame_body_size(const Frame& f) {
    std::size_t n = kFrameHead;
    for (const FrameRect& r : f.rects) n += rect_size(r);
    return n;
}

std::vector<Frame> split_frame(Frame f, std::size_t max_body) {
    std::vector<Frame> out;
    if (frame_body_size(f) <= max_body) {
        out.push_back(std::move(f));
        return out;
    }
    auto fresh = [&] {
        Frame g;
        g.tab = f.tab;
        g.layer = f.layer;
        g.width = f.width;
        g.height = f.height;
        return g;
    };
    Frame cur = fresh();
    std::size_t size = kFrameHead;
    auto add = [&](FrameRect r) {
        if (!cur.rects.empty() && size + rect_size(r) > max_body) {
            out.push_back(std::move(cur));
            cur = fresh();
            size = kFrameHead;
        }
        size += rect_size(r);
        cur.rects.push_back(std::move(r));
    };
    for (FrameRect& r : f.rects) {
        if (kFrameHead + rect_size(r) <= max_body) {
            add(std::move(r));
            continue;
        }
        // Bands of whole rows, each a frame of its own.
        const std::size_t row = std::size_t{r.width} * 4;
        const std::size_t rows = std::max<std::size_t>(1, (max_body - kFrameHead - kRectHead) / row);
        for (std::size_t y = 0; y < r.height; y += rows) {
            const std::size_t n = std::min<std::size_t>(rows, r.height - y);
            FrameRect band;
            band.x = r.x;
            band.y = static_cast<std::uint16_t>(r.y + y);
            band.width = r.width;
            band.height = static_cast<std::uint16_t>(n);
            const auto from = r.bgra.begin() + static_cast<std::ptrdiff_t>(y * row);
            band.bgra.assign(from, from + static_cast<std::ptrdiff_t>(n * row));
            add(std::move(band));
        }
        r.bgra = {};
    }
    if (!cur.rects.empty()) out.push_back(std::move(cur));
    return out;
}

void cut_utf8(std::string& s, std::size_t max) {
    if (s.size() <= max) return;
    std::size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xc0) == 0x80) --n;
    s.resize(n);
}

std::vector<std::byte> encode_message(const Message& m) {
    ByteWriter w;
    w.le(std::uint32_t{0}); // the length, filled in below
    w.le(static_cast<std::uint8_t>(m.index() + 1));
    std::visit([&](const auto& v) { write(w, v); }, m);
    std::vector<std::byte> out = w.take();
    const auto len = static_cast<std::uint32_t>(out.size() - 4);
    for (std::size_t i = 0; i < 4; ++i) out[i] = static_cast<std::byte>((len >> (8 * i)) & 0xff);
    return out;
}

std::optional<Message> decode_message(std::span<const std::byte> body) {
    try {
        ByteReader r(body);
        auto type = r.le<std::uint8_t>();
        if (type == 0 || body.size() > max_message_size(type)) return std::nullopt;
        auto m = read_index<0>(std::size_t{type} - 1, r);
        if (!m || !r.empty()) return std::nullopt;
        return m;
    } catch (const std::out_of_range&) {
        return std::nullopt;
    } catch (const std::length_error&) {
        return std::nullopt;
    }
}

void MessageReader::feed(std::span<const std::byte> bytes) {
    if (failed_) return;
    if (pos_ > 0 && pos_ >= buf_.size() / 2) { // drop what has been read before the buffer grows
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
        pos_ = 0;
    }
    buf_.insert(buf_.end(), bytes.begin(), bytes.end());
}

std::optional<Message> MessageReader::next() {
    if (failed_ || buf_.size() - pos_ < 4) return std::nullopt;
    std::uint32_t len = 0;
    for (std::size_t i = 0; i < 4; ++i) len |= std::to_integer<std::uint32_t>(buf_[pos_ + i]) << (8 * i);
    if (len == 0 || len > kMaxMessage) {
        failed_ = true;
        return std::nullopt;
    }
    // A console line or a page's message as long as a frame: broken, without waiting for all of it.
    if (buf_.size() - pos_ > 4 && len > max_message_size(std::to_integer<std::uint8_t>(buf_[pos_ + 4]))) {
        failed_ = true;
        return std::nullopt;
    }
    if (buf_.size() - pos_ - 4 < len) return std::nullopt;
    auto m = decode_message(std::span<const std::byte>(buf_).subspan(pos_ + 4, len));
    if (!m) {
        failed_ = true;
        return std::nullopt;
    }
    pos_ += 4 + len;
    return m;
}

} // namespace hailstone::web
