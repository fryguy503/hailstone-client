#pragma once

// The messages between the client and its browser helper (tools/hailstone-web, docs/browser.md), over the helper's
// standard input (client -> helper) and output (helper -> client). Each message is a u32 length (little-endian) and
// that many bytes: a u8 type and its fields. Integers are little-endian; a string is a u32 length and its UTF-8
// bytes. Only the SDK's byte reader and writer, so the helper builds this file without the rest of the client.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "web/page_rules.h"

namespace hailstone::web {

inline constexpr std::uint32_t kProtocolVersion = 2; // 2: Open carries the tab's rules (TabRules)
// The largest message either side accepts: a 4K page's full frame is 33 MB. A larger paint (a 4096x4096 tab's is
// just over 64 MiB) goes as several Frames (split_frame).
inline constexpr std::size_t kMaxMessage = std::size_t{64} << 20;
// What the page decides the size of, smaller: a console line's text (the helper cuts it, at a UTF-8 character) and
// a window.hailstone.post() message (the page gets an exception for a longer one). A Console or PageMessage longer
// than these allow is a broken message (max_message_size).
inline constexpr std::size_t kMaxConsoleText = 4096;
inline constexpr std::size_t kMaxPageMessage = std::size_t{1} << 20;
// What the client sends, held to limits of its own before it goes (BrowserHost): an address at most Chromium's own
// limit (url::kMaxURLChars, 2 MB), a Post to the page at most what the page may post back, and any message at most
// kMaxMessage, which the helper would take as a broken stream and quit on.
inline constexpr std::size_t kMaxUrl = std::size_t{2} << 20;
inline constexpr std::size_t kMaxPostMessage = kMaxPageMessage;

// ---- client -> helper ----
struct Hello {
    std::uint32_t version = kProtocolVersion;
};
struct Open {
    std::uint32_t tab = 0;
    std::uint16_t width = 0, height = 0;
    // The rules the helper holds the tab to at every navigation and request (web/page_rules.h): the player's, a
    // server link's (no private hosts), or a script's or plugin's (on `allow`, core/web_url.h; empty: any host).
    TabRules rules = TabRules::Player;
    std::vector<std::string> allow;
    std::string profile; // the cookie and cache folder (UTF-8 path)
    std::string url;
};
struct Navigate {
    std::uint32_t tab = 0;
    std::string url;
};
struct Resize {
    std::uint32_t tab = 0;
    std::uint16_t width = 0, height = 0;
};
enum class MouseKind : std::uint8_t { Move = 0, Down = 1, Up = 2, Leave = 3 };
enum class MouseButton : std::uint8_t { Left = 0, Middle = 1, Right = 2 };
// Modifier bits (Mouse, Wheel, Key).
inline constexpr std::uint32_t kModShift = 1, kModCtrl = 2, kModAlt = 4, kModMeta = 8;
// The mouse buttons held (a drag selects text).
inline constexpr std::uint32_t kModLeftButton = 16, kModMiddleButton = 32, kModRightButton = 64;
struct Mouse {
    std::uint32_t tab = 0;
    MouseKind kind = MouseKind::Move;
    MouseButton button = MouseButton::Left;
    std::uint8_t clicks = 1;
    std::int32_t x = 0, y = 0; // the page's pixels
    std::uint32_t mods = 0;
};
struct Wheel {
    std::uint32_t tab = 0;
    std::int32_t x = 0, y = 0, dx = 0, dy = 0; // dx, dy in pixels
    std::uint32_t mods = 0;
};
enum class KeyKind : std::uint8_t { Down = 0, Up = 1, Char = 2 };
struct Key {
    std::uint32_t tab = 0;
    KeyKind kind = KeyKind::Down;
    std::uint32_t code = 0;      // the Windows virtual-key code (what CEF takes on every platform)
    std::uint32_t character = 0; // Char: the UTF-32 character
    std::uint32_t mods = 0;
};
struct Text {
    std::uint32_t tab = 0;
    std::string text;
};
struct Focus {
    std::uint32_t tab = 0;
    bool on = false;
};
enum class NavAction : std::uint8_t { Back = 0, Forward = 1, Reload = 2, Stop = 3 };
struct Nav {
    std::uint32_t tab = 0;
    NavAction action = NavAction::Reload;
};
struct Eval {
    std::uint32_t tab = 0;
    std::string script;
};
struct Post { // to the page's window.hailstone.onmessage
    std::uint32_t tab = 0;
    std::string message;
};
struct Close {
    std::uint32_t tab = 0;
};
struct Shutdown {};

// ---- helper -> client ----
struct Ready {
    std::uint32_t version = kProtocolVersion;
};
struct FrameRect {
    std::uint16_t x = 0, y = 0, width = 0, height = 0;
    std::vector<std::byte> bgra; // width * height * 4, rows top to bottom
};
enum class Layer : std::uint8_t { View = 0, Popup = 1 };
// The changed parts of a tab's image, `width` x `height` in all.
struct Frame {
    std::uint32_t tab = 0;
    Layer layer = Layer::View;
    std::uint16_t width = 0, height = 0;
    std::vector<FrameRect> rects;
};
struct Popup { // a select list's popup: where its image (Layer::Popup) goes over the view
    std::uint32_t tab = 0;
    bool shown = false;
    std::int32_t x = 0, y = 0;
    std::uint16_t width = 0, height = 0;
};
struct Address {
    std::uint32_t tab = 0;
    std::string url;
};
struct Title {
    std::uint32_t tab = 0;
    std::string title;
};
struct Loading {
    std::uint32_t tab = 0;
    bool loading = false, can_back = false, can_forward = false;
};
enum class Cursor : std::uint8_t { Arrow = 0, Hand = 1, Text = 2, Wait = 3, Crosshair = 4, ResizeEW = 5, ResizeNS = 6, Move = 7, NotAllowed = 8 };
struct CursorShape {
    std::uint32_t tab = 0;
    Cursor cursor = Cursor::Arrow;
};
struct Console {
    std::uint32_t tab = 0;
    std::uint8_t level = 0; // 0 debug .. 3 error
    std::string text;
};
struct PageMessage { // the page's window.hailstone.post()
    std::uint32_t tab = 0;
    std::string message;
};
enum class CloseReason : std::uint8_t { Asked = 0, Refused = 1, Crashed = 2, Failed = 3 };
struct Closed {
    std::uint32_t tab = 0;
    CloseReason reason = CloseReason::Asked;
    std::string detail;
};

using Message = std::variant<Hello, Open, Navigate, Resize, Mouse, Wheel, Key, Text, Focus, Nav, Eval, Post, Close,
                             Shutdown, Ready, Frame, Popup, Address, Title, Loading, CursorShape, Console, PageMessage,
                             Closed>;

// One message with its length in front. The sender checks the body against max_message_size before it sends it
// (BrowserHost::send): a longer one is not to be sent.
std::vector<std::byte> encode_message(const Message& m);
// A message's bytes after the length; nullopt for an unknown type, a short message, bytes left over or values out
// of range (a frame rectangle outside its image).
std::optional<Message> decode_message(std::span<const std::byte> body);

// A Frame as Frames whose bodies are each at most `max_body` bytes (kMaxMessage): rectangles grouped while they fit,
// and one too large on its own cut into bands of whole rows. The client copies each in turn, so the image is the
// same. `max_body` must hold a frame of one row of the widest rectangle.
std::vector<Frame> split_frame(Frame f, std::size_t max_body = kMaxMessage);
// The body size of a Frame message (the type byte and its fields).
std::size_t frame_body_size(const Frame& f);

// The largest body (after the length) a message of this type byte may have: kMaxMessage, less for Console and
// PageMessage.
std::size_t max_message_size(std::uint8_t type);

// Cuts `s` to at most `max` bytes, not inside a UTF-8 character.
void cut_utf8(std::string& s, std::size_t max);

// Messages out of a byte stream that arrives in pieces.
class MessageReader {
public:
    void feed(std::span<const std::byte> bytes);
    // The next whole message; nullopt when none is complete yet or the stream is broken (failed()).
    std::optional<Message> next();
    // A length over kMaxMessage (or its type's max_message_size) or zero, or a message that does not decode: nothing
    // more is read.
    bool failed() const { return failed_; }

private:
    std::vector<std::byte> buf_;
    std::size_t pos_ = 0;
    bool failed_ = false;
};

} // namespace hailstone::web
