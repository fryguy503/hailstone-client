// A stand-in for the browser helper (docs/browser.md) for tests/browser_host_test.cpp: it speaks the protocol over
// the pipes it was started with, taken off its standard input and output as the real helper does
// (platform::take_protocol_stdio), and draws no web page. Each tab's image is a test pattern (blue = x, green = y,
// red = the tab id); a left click paints its pixel white. What it does for the tests:
//   Open, Navigate   the address back, a title "Mock <address>", loading done; an address with "redirect=" in it
//                    lands on what follows (a redirect off the site)
//   Resize           the whole image again
//   Post             the message back, as the page's, with "echo:" in front
//   Eval             "crash": exit 3; "garbage": bytes that are no message; "console": a console line; "close":
//                    Closed (failed); "child": runs itself with --print, which writes bytes that are no message to
//                    its standard output, then a console line "child done"; "streams": a console line saying
//                    whether a process it starts would inherit its pipes ("in:0 out:0" when not)
//   Shutdown, end of input: exit 0
// "--version N" answers Hello with that version; "--stall 1" never reads its input (a helper that is stuck);
// "--print" writes junk to standard output and exits (the child above).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "platform/binary_stdio.h"
#include "web/browser_protocol.h"

using namespace hailstone::web;

namespace {

hailstone::platform::ProtocolStreams g_client;

void out(const Message& m) {
    std::vector<std::byte> b = encode_message(m);
    std::fwrite(b.data(), 1, b.size(), g_client.out);
    std::fflush(g_client.out);
}

bool read_exact(void* p, std::size_t n) { return std::fread(p, 1, n, g_client.in) == n; }

// What a page's process inheriting the pipe could do: bytes that are no message, on its standard output.
void run_child(const char* self) {
    std::string command = std::string("\"") + self + "\" --print";
#if defined(_WIN32)
    command = "\"" + command + "\""; // cmd /c strips the outer pair
#endif
    std::fflush(nullptr);
    [[maybe_unused]] const int code = std::system(command.c_str());
}

Frame pattern(std::uint32_t tab, std::uint16_t w, std::uint16_t h) {
    Frame f;
    f.tab = tab;
    f.width = w;
    f.height = h;
    FrameRect r;
    r.width = w;
    r.height = h;
    r.bgra.resize(std::size_t{w} * h * 4);
    for (std::size_t y = 0; y < h; ++y)
        for (std::size_t x = 0; x < w; ++x) {
            std::byte* px = r.bgra.data() + (y * w + x) * 4;
            px[0] = static_cast<std::byte>(x & 0xff);
            px[1] = static_cast<std::byte>(y & 0xff);
            px[2] = static_cast<std::byte>(tab & 0xff);
            px[3] = std::byte{0xff};
        }
    f.rects.push_back(std::move(r));
    return f;
}

void loaded(std::uint32_t tab, std::string url) {
    if (auto at = url.find("redirect="); at != std::string::npos) url = url.substr(at + 9);
    out(Address{tab, url});
    out(Title{tab, "Mock " + url});
    out(Loading{tab, false, false, false});
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--print") == 0) {
            std::fputs("\xff\xff\xff\x7fnot a message", stdout);
            std::fflush(stdout);
            return 0;
        }
    if (!hailstone::platform::take_protocol_stdio(g_client)) return 4;
    std::uint32_t version = kProtocolVersion;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--version") == 0) version = static_cast<std::uint32_t>(std::atoi(argv[i + 1]));
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--stall") == 0)
            for (;;) std::this_thread::sleep_for(std::chrono::seconds(1)); // until it is ended
    struct Size {
        std::uint16_t w = 0, h = 0;
    };
    std::vector<std::pair<std::uint32_t, Size>> sizes;
    auto size_of = [&](std::uint32_t tab) -> Size& {
        for (auto& [id, s] : sizes)
            if (id == tab) return s;
        return sizes.emplace_back(tab, Size{}).second;
    };
    for (;;) {
        unsigned char len_bytes[4];
        if (!read_exact(len_bytes, 4)) return 0;
        const std::uint32_t len = std::uint32_t{len_bytes[0]} | std::uint32_t{len_bytes[1]} << 8 |
                                  std::uint32_t{len_bytes[2]} << 16 | std::uint32_t{len_bytes[3]} << 24;
        if (len == 0 || len > kMaxMessage) return 2;
        std::vector<std::byte> body(len);
        if (!read_exact(body.data(), len)) return 0;
        auto m = decode_message(body);
        if (!m) return 2;
        if (std::get_if<Hello>(&*m)) {
            out(Ready{version});
        } else if (auto* o = std::get_if<Open>(&*m)) {
            size_of(o->tab) = {o->width, o->height};
            loaded(o->tab, o->url);
            out(pattern(o->tab, o->width, o->height));
        } else if (auto* n = std::get_if<Navigate>(&*m)) {
            loaded(n->tab, n->url);
        } else if (auto* r = std::get_if<Resize>(&*m)) {
            size_of(r->tab) = {r->width, r->height};
            out(pattern(r->tab, r->width, r->height));
        } else if (auto* mo = std::get_if<Mouse>(&*m)) {
            Size s = size_of(mo->tab);
            if (mo->kind == MouseKind::Down && mo->x >= 0 && mo->y >= 0 && mo->x < s.w && mo->y < s.h) {
                Frame f;
                f.tab = mo->tab;
                f.width = s.w;
                f.height = s.h;
                FrameRect px;
                px.x = static_cast<std::uint16_t>(mo->x);
                px.y = static_cast<std::uint16_t>(mo->y);
                px.width = px.height = 1;
                px.bgra.assign(4, std::byte{0xff});
                f.rects.push_back(std::move(px));
                out(f);
            }
        } else if (auto* p = std::get_if<Post>(&*m)) {
            out(PageMessage{p->tab, "echo:" + p->message});
        } else if (auto* e = std::get_if<Eval>(&*m)) {
            if (e->script == "crash") return 3;
            if (e->script == "garbage") {
                const unsigned char junk[] = {5, 0, 0, 0, 0xee, 1, 2, 3, 4};
                std::fwrite(junk, 1, sizeof junk, g_client.out);
                std::fflush(g_client.out);
            } else if (e->script == "console") {
                out(Console{e->tab, 2, "a warning"});
            } else if (e->script == "close") {
                out(Closed{e->tab, CloseReason::Failed, "the page failed"});
            } else if (e->script == "child") {
                run_child(argv[0]);
                out(Console{e->tab, 1, "child done"});
            } else if (e->script == "streams") {
                const bool in = hailstone::platform::stream_inherited_by_children(g_client.in);
                const bool out_inherited = hailstone::platform::stream_inherited_by_children(g_client.out);
                out(Console{e->tab, 1, std::string("in:") + (in ? "1" : "0") + " out:" + (out_inherited ? "1" : "0")});
            }
        } else if (std::get_if<Shutdown>(&*m)) {
            return 0;
        }
    }
}
