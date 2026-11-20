#include "app/web_service.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <optional>

#include "plugin/policy.h"
#include "ui/web_textures.h"
#include "web/browser_host.h"

namespace hailstone::app {

namespace {

std::uint32_t mods_now() {
    const ImGuiIO& io = ImGui::GetIO();
    std::uint32_t m = 0;
    if (io.KeyShift) m |= web::kModShift;
    if (io.KeyCtrl) m |= web::kModCtrl;
    if (io.KeyAlt) m |= web::kModAlt;
    if (io.KeySuper) m |= web::kModMeta;
    if (io.MouseDown[0]) m |= web::kModLeftButton;
    if (io.MouseDown[1]) m |= web::kModRightButton;
    if (io.MouseDown[2]) m |= web::kModMiddleButton;
    return m;
}

// The keys a page needs as keys (text comes as characters), with their Windows virtual-key codes.
struct KeyCode {
    ImGuiKey key;
    std::uint32_t vk;
};
constexpr KeyCode kKeys[] = {
    {ImGuiKey_Backspace, 0x08}, {ImGuiKey_Tab, 0x09},       {ImGuiKey_Enter, 0x0d},      {ImGuiKey_KeypadEnter, 0x0d},
    {ImGuiKey_Escape, 0x1b},    {ImGuiKey_PageUp, 0x21},    {ImGuiKey_PageDown, 0x22},   {ImGuiKey_End, 0x23},
    {ImGuiKey_Home, 0x24},      {ImGuiKey_LeftArrow, 0x25}, {ImGuiKey_UpArrow, 0x26},    {ImGuiKey_RightArrow, 0x27},
    {ImGuiKey_DownArrow, 0x28}, {ImGuiKey_Insert, 0x2d},    {ImGuiKey_Delete, 0x2e},     {ImGuiKey_F5, 0x74},
};

void send_keys(web::BrowserHost& b, std::uint32_t tab) {
    const std::uint32_t mods = mods_now();
    for (const KeyCode& k : kKeys) {
        if (ImGui::IsKeyPressed(k.key, true)) {
            b.key({tab, web::KeyKind::Down, k.vk, 0, mods});
            if (k.vk == 0x0d) b.key({tab, web::KeyKind::Char, k.vk, '\r', mods}); // forms submit on the character
        }
        if (ImGui::IsKeyReleased(k.key)) b.key({tab, web::KeyKind::Up, k.vk, 0, mods});
    }
    // Ctrl / Cmd + a letter (copy, paste, select all ...): no text comes for them.
    if (mods & (web::kModCtrl | web::kModMeta))
        for (int i = 0; i < 26; ++i) {
            const auto key = static_cast<ImGuiKey>(ImGuiKey_A + i);
            const std::uint32_t vk = 0x41u + static_cast<std::uint32_t>(i);
            if (ImGui::IsKeyPressed(key, true)) b.key({tab, web::KeyKind::Down, vk, 0, mods});
            if (ImGui::IsKeyReleased(key)) b.key({tab, web::KeyKind::Up, vk, 0, mods});
        }
    // Typed text, one UTF-16 unit at a time as CEF takes it.
    for (ImWchar c : ImGui::GetIO().InputQueueCharacters) {
        std::uint32_t cp = c;
        if (cp < 0x20 || cp == 0x7f) continue;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            b.key({tab, web::KeyKind::Char, 0, 0xd800 + (cp >> 10), mods});
            b.key({tab, web::KeyKind::Char, 0, 0xdc00 + (cp & 0x3ff), mods});
        } else {
            b.key({tab, web::KeyKind::Char, 0, cp, mods});
        }
    }
}

ImGuiMouseCursor imgui_cursor(web::Cursor c) {
    switch (c) {
    case web::Cursor::Hand: return ImGuiMouseCursor_Hand;
    case web::Cursor::Text: return ImGuiMouseCursor_TextInput;
    case web::Cursor::ResizeEW: return ImGuiMouseCursor_ResizeEW;
    case web::Cursor::ResizeNS: return ImGuiMouseCursor_ResizeNS;
    case web::Cursor::Move: return ImGuiMouseCursor_ResizeAll;
    case web::Cursor::NotAllowed: return ImGuiMouseCursor_NotAllowed;
    default: return ImGuiMouseCursor_Arrow;
    }
}

} // namespace

std::string WebService::address(std::string text) {
    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
    while (!text.empty() && text.back() == ' ') text.pop_back();
    if (!text.empty() && text.find("://") == std::string::npos && text != "about:blank") text = "https://" + text;
    return text;
}

std::string WebService::owner_profile(const std::string& base, const std::string& owner) {
    if (owner.empty() || base.empty()) return base;
    // Owners' names match without case (a plugin's, a script's), so one folder for each spelling. A name is any
    // text: what is not a letter, a digit, '-' or '_' becomes '_', and its hash (FNV-1a) keeps two names apart.
    std::string lower;
    for (char c : owner) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::uint32_t h = 2166136261u;
    for (unsigned char c : lower) h = (h ^ c) * 16777619u;
    std::string slug;
    for (char c : lower.substr(0, 64)) slug += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_';
    std::string out = base;
    if (out.back() != '/' && out.back() != '\\') out += '/';
    return out + std::format("hailstone-addons/{}-{:08x}", slug, h);
}

web::BrowserHost* WebService::host() { return env_.browser ? env_.browser() : nullptr; }

bool WebService::may(const std::string& owner, std::uint32_t caps, std::string* why) {
    if (owner.empty() || (caps & plugin::kCapWeb)) return true;
    if (why) *why = "web pages need the server's 'web' permission";
    return false;
}

bool WebService::mine(const std::string& owner, std::uint32_t tab) {
    web::BrowserHost* b = host();
    const web::Tab* t = b ? b->tab(tab) : nullptr;
    return t && !t->closed && t->options.owner == owner;
}

bool WebService::available() {
    web::BrowserHost* b = host();
    return b && b->available();
}

std::uint32_t WebService::open(const std::string& owner, std::uint32_t caps, const std::string& url, int width,
                               int height, std::string* why) {
    if (!may(owner, caps, why)) return 0;
    web::BrowserHost* b = host();
    if (!b || !b->available()) {
        if (why) *why = b && !b->last_error().empty() ? b->last_error() : "the in-game browser is not installed";
        return 0;
    }
    if (!owner.empty() && tabs(owner).size() >= kMaxTabsPerOwner) {
        if (why) *why = std::format("at most {} tabs may be open at once: close one first", kMaxTabsPerOwner);
        return 0;
    }
    web::TabOptions o;
    o.width = static_cast<std::uint16_t>(std::clamp(width, 16, 4096));
    o.height = static_cast<std::uint16_t>(std::clamp(height, 16, 4096));
    o.owner = owner;
    if (!owner.empty() && env_.allow) o.allow = env_.allow();
    if (env_.profile) o.profile = owner_profile(env_.profile(), owner);
    return b->open(address(url), std::move(o), why);
}

std::uint32_t WebService::open_server_link(const std::string& url, std::string* why) {
    web::BrowserHost* b = host();
    if (!b || !b->available()) {
        if (why) *why = b && !b->last_error().empty() ? b->last_error() : "the in-game browser is not installed";
        return 0;
    }
    web::TabOptions o;
    o.server_link = true; // no private host at any navigation or request, not only this first address
    if (env_.profile) o.profile = server_link_profile(env_.profile());
    return b->open(url, std::move(o), why);
}

std::string WebService::server_link_profile(const std::string& base) {
    if (base.empty()) return base;
    std::string out = base;
    if (out.back() != '/' && out.back() != '\\') out += '/';
    return out + "server-links";
}

bool WebService::navigate(const std::string& owner, std::uint32_t caps, std::uint32_t tab, const std::string& url,
                          std::string* why) {
    if (!may(owner, caps, why)) return false;
    if (!mine(owner, tab)) {
        if (why) *why = "no such tab";
        return false;
    }
    return host()->navigate(tab, address(url), why);
}

bool WebService::nav(const std::string& owner, std::uint32_t caps, std::uint32_t tab, const std::string& action) {
    if (!may(owner, caps) || !mine(owner, tab)) return false;
    web::NavAction a;
    if (action == "back") a = web::NavAction::Back;
    else if (action == "forward") a = web::NavAction::Forward;
    else if (action == "reload") a = web::NavAction::Reload;
    else if (action == "stop") a = web::NavAction::Stop;
    else return false;
    return host()->nav(tab, a, owner.empty());
}

void WebService::close(const std::string& owner, std::uint32_t tab) {
    web::BrowserHost* b = host();
    const web::Tab* t = b ? b->tab(tab) : nullptr;
    if (t && t->options.owner == owner) b->close(tab); // closed ones too: they are forgotten
    if (focused_ == tab) focused_ = 0;
}

bool WebService::eval(const std::string& owner, std::uint32_t caps, std::uint32_t tab, const std::string& script) {
    if (!may(owner, caps) || !mine(owner, tab)) return false;
    return host()->eval(tab, script);
}

bool WebService::post(const std::string& owner, std::uint32_t caps, std::uint32_t tab, const std::string& message) {
    if (!may(owner, caps) || !mine(owner, tab)) return false;
    return host()->post(tab, message);
}

bool WebService::text(const std::string& owner, std::uint32_t caps, std::uint32_t tab, const std::string& text) {
    if (!may(owner, caps) || !mine(owner, tab)) return false;
    return host()->text(tab, text);
}

std::optional<std::string> WebService::take_message(const std::string& owner, std::uint32_t tab) {
    web::BrowserHost* b = host();
    const web::Tab* t = b ? b->tab(tab) : nullptr;
    if (!t || owner.empty() || t->options.owner != owner) return std::nullopt;
    return b->take_message(tab);
}

std::optional<std::size_t> WebService::next_message_size(const std::string& owner, std::uint32_t tab) {
    web::BrowserHost* b = host();
    const web::Tab* t = b ? b->tab(tab) : nullptr;
    if (!t || owner.empty() || t->options.owner != owner || t->messages.empty()) return std::nullopt;
    return t->messages.front().size();
}

std::optional<plugin::WebState> WebService::state(const std::string& owner, std::uint32_t tab) {
    web::BrowserHost* b = host();
    const web::Tab* t = b ? b->tab(tab) : nullptr;
    if (!t || t->options.owner != owner) return std::nullopt;
    plugin::WebState s;
    s.url = t->url;
    s.title = t->title;
    s.loading = t->loading;
    s.can_back = t->can_back;
    s.can_forward = t->can_forward;
    s.closed = t->closed;
    s.why = t->close_detail;
    return s;
}

std::vector<std::uint32_t> WebService::tabs(const std::string& owner) {
    std::vector<std::uint32_t> out;
    web::BrowserHost* b = host();
    if (!b) return out;
    for (std::uint32_t id : b->tabs())
        if (const web::Tab* t = b->tab(id); t && t->options.owner == owner) out.push_back(id);
    return out;
}

void WebService::close_owner(const std::string& owner) {
    web::BrowserHost* b = host();
    if (!b) return;
    for (std::uint32_t id : tabs(owner)) {
        b->close(id);
        if (focused_ == id) focused_ = 0;
    }
}

void WebService::recheck(const std::function<std::uint32_t(const std::string&)>& caps_of) {
    web::BrowserHost* b = host();
    if (!b) return;
    std::optional<std::vector<std::string>> allow; // read once, and only with a script's tab open
    for (std::uint32_t id : b->tabs()) {
        const web::Tab* t = b->tab(id);
        if (!t || t->closed || t->options.owner.empty()) continue;
        std::string why;
        if (!(caps_of(t->options.owner) & plugin::kCapWeb)) {
            why = "it no longer has the server's 'web' permission";
        } else {
            if (!allow) allow = env_.allow ? env_.allow() : std::vector<std::string>{};
            if (t->options.allow != *allow) why = "the server changed the sites scripts may open";
        }
        if (why.empty()) continue;
        b->refuse(id, std::move(why));
        if (focused_ == id) focused_ = 0;
    }
}

bool WebService::view(const std::string& owner, std::uint32_t caps, std::uint32_t tab, float width, float height) {
    const float w = std::max(16.0f, width), h = std::max(16.0f, height);
    ImGui::PushID(static_cast<int>(tab));
    ImGui::InvisibleButton("##page", ImVec2(w, h),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    ImGui::PopID();
    const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!may(owner, caps) || !mine(owner, tab)) {
        dl->AddRectFilled(p0, p1, IM_COL32(24, 24, 24, 255));
        return hovered;
    }
    web::BrowserHost& b = *host();
    const web::Tab& t = *b.tab(tab);
    b.resize(tab, static_cast<std::uint16_t>(std::min(w, 4096.0f)), static_cast<std::uint16_t>(std::min(h, 4096.0f)));
    ui::WebTextures* textures = env_.textures ? env_.textures() : nullptr;
    const auto view = textures ? textures->texture(tab) : ui::WebTextures::View{};
    if (view.id)
        dl->AddImage(reinterpret_cast<ImTextureID>(view.id), p0,
                     ImVec2(p0.x + static_cast<float>(view.width), p0.y + static_cast<float>(view.height)));
    else
        dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 255));
    if (textures && t.popup_shown)
        if (const auto popup = textures->texture(tab, web::Layer::Popup); popup.id) {
            const ImVec2 q0(p0.x + static_cast<float>(t.popup_x), p0.y + static_cast<float>(t.popup_y));
            ImGui::GetForegroundDrawList()->AddImage(
                reinterpret_cast<ImTextureID>(popup.id), q0,
                ImVec2(q0.x + static_cast<float>(popup.width), q0.y + static_cast<float>(popup.height)));
        }

    const ImGuiIO& io = ImGui::GetIO();
    // With no mouse (it left the game's window) ImGui's position is -FLT_MAX, which no int holds: the last one the
    // page had stands in, and every position is held to a range an int32 takes.
    const bool valid = ImGui::IsMousePosValid();
    const float mx = valid ? io.MousePos.x : last_x_, my = valid ? io.MousePos.y : last_y_;
    auto page = [](float v) { return static_cast<std::int32_t>(std::clamp(v, -65536.0f, 65536.0f)); };
    const std::int32_t x = page(mx - p0.x), y = page(my - p0.y);
    const std::uint32_t mods = mods_now();
    if (hovered || ImGui::IsItemActive()) {
        if (valid && (mx != last_x_ || my != last_y_)) b.mouse({tab, web::MouseKind::Move, {}, 1, x, y, mods});
        last_x_ = mx;
        last_y_ = my;
        hovered_ = tab;
        ImGui::SetMouseCursor(imgui_cursor(t.cursor));
        constexpr web::MouseButton kButtons[] = {web::MouseButton::Left, web::MouseButton::Right, web::MouseButton::Middle};
        for (int i = 0; i < 3; ++i) {
            const std::uint8_t clicks = ImGui::IsMouseDoubleClicked(i) ? 2 : 1;
            if (hovered && ImGui::IsMouseClicked(i)) {
                b.mouse({tab, web::MouseKind::Down, kButtons[i], clicks, x, y, mods});
                if (focused_ != tab) {
                    if (focused_ && b.tab(focused_)) b.focus(focused_, false);
                    focused_ = tab;
                    b.focus(tab, true);
                }
            }
            if (ImGui::IsMouseReleased(i)) b.mouse({tab, web::MouseKind::Up, kButtons[i], clicks, x, y, mods});
        }
        if (hovered && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f))
            b.wheel({tab, x, y, static_cast<std::int32_t>(-io.MouseWheelH * 100.0f),
                     static_cast<std::int32_t>(io.MouseWheel * 100.0f), mods});
    } else if (hovered_ == tab) {
        b.mouse({tab, web::MouseKind::Leave, {}, 1, x, y, mods});
        hovered_ = 0;
    }
    // A click anywhere else takes the keyboard away from the page.
    if (focused_ == tab && !hovered && (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1))) {
        b.focus(tab, false);
        focused_ = 0;
    }
    if (focused_ == tab) {
        ImGui::SetNextFrameWantCaptureKeyboard(true); // the game's key binds stay off while the page has the keys
        if (env_.wants_text) env_.wants_text();
        send_keys(b, tab);
        dl->AddRect(p0, p1, IM_COL32(90, 140, 220, 255));
    }
    return hovered;
}

} // namespace hailstone::app
