// ui.CodeEditor: a Lua text box with colours (ui/code_syntax.h), an error line and completion, for the Lua social
// editor. ImGui's multiline InputText edits the text; its own letters are drawn clear and the coloured spans are
// drawn in their place, so the box keeps InputText's editing, selection and undo. The box is as big as its text and
// scrolls inside a child window, so the spans and the box's letters stay in the same places.
//
//   text, changed, cursor, active = ui.CodeEditor(id, text, w, h [, options])
//   options: error_line = n (marked red), read_only = true,
//            complete = function(before) -> start, {names}: the names that may replace the text from byte `start`
//            (1-based) to the cursor; asked when the text or the cursor moves. Tab takes the highlighted name,
//            Ctrl+Up / Ctrl+Down move the highlight, a click takes one.
//   cursor: the byte offset of the cursor (0 = before the first byte).
//
// Also ui.SetClipboardText(text) and ui.GetClipboardText(); a host may refuse them to a state (check_clipboard).

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "ui/code_syntax.h"
#include "ui/imgui_frame.h"
#include "ui/lua/lua_host.h"

#include <lua.hpp>

namespace hailstone::ui::lua {

namespace {

struct EditorState {
    int cursor = 0, last_cursor = -1;
    std::string asked; // the text before the cursor the names were asked for
    int start = 0;     // where the names replace from
    std::vector<std::string> names;
    int selected = 0;
    bool accept = false;
    bool hovered = false; // the names' window had the mouse last frame (a click there takes the box's focus)
    ImGuiID input = 0;
    std::uint64_t seen = 0; // the frame it was last drawn (ui::imgui_frame, from 1)
    std::uint64_t made = 0; // the frame it was first drawn; 0 not yet
};

// An editor not drawn for this many frames is forgotten; past kMaxEditors (ids made from changing text) every one
// not drawn last frame is (the sweep runs before this frame's first editor: those drawn last frame stay). The map is C++ memory, outside every Lua memory limit.
constexpr std::uint64_t kForgetFrames = 3600;
constexpr std::size_t kMaxEditors = 256;

std::unordered_map<ImGuiID, EditorState>& editors() {
    static std::unordered_map<ImGuiID, EditorState> e;
    return e;
}

// Once a frame, before any editor of it is looked up (a reference into the map stays valid until then: the map only
// erases here, and a nested ui.CodeEditor in a completion function only adds).
void forget_editors(std::uint64_t frame) {
    static std::uint64_t swept = 0;
    if (frame == swept) return;
    swept = frame;
    auto& e = editors();
    const bool over = e.size() > kMaxEditors;
    std::erase_if(e, [&](const auto& kv) {
        const std::uint64_t seen = kv.second.seen;
        return seen > frame || frame - seen > (over ? std::uint64_t{1} : kForgetFrames);
    });
}

struct Callback {
    std::string* text;
    EditorState* st;
    bool showing; // the names show
};

int callback(ImGuiInputTextCallbackData* d) {
    auto* c = static_cast<Callback*>(d->UserData);
    EditorState& st = *c->st;
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        c->text->resize(static_cast<std::size_t>(d->BufTextLen));
        d->Buf = c->text->data();
    } else if (d->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
        if (d->EventChar == '\t' && c->showing) {
            st.accept = true;
            return 1;
        }
    } else if (d->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        if (st.accept && st.selected >= 0 && st.selected < static_cast<int>(st.names.size()) && st.start <= d->CursorPos &&
            d->CursorPos <= d->BufTextLen) {
            d->DeleteChars(st.start, d->CursorPos - st.start);
            d->InsertChars(st.start, st.names[static_cast<std::size_t>(st.selected)].c_str());
            st.names.clear();
        }
        st.accept = false;
        st.cursor = d->CursorPos;
    }
    return 0;
}

ImU32 token_color(CodeToken k) {
    switch (k) {
    case CodeToken::Keyword: return IM_COL32(110, 160, 255, 255);
    case CodeToken::String: return IM_COL32(230, 160, 90, 255);
    case CodeToken::Comment: return IM_COL32(110, 160, 110, 255);
    case CodeToken::Number: return IM_COL32(180, 220, 140, 255);
    case CodeToken::Builtin: return IM_COL32(200, 140, 230, 255);
    case CodeToken::Member: return IM_COL32(110, 210, 200, 255);
    case CodeToken::Symbol: return IM_COL32(190, 190, 190, 255);
    case CodeToken::Name: break;
    }
    return IM_COL32(230, 230, 230, 255);
}

// The line and the x of bytes of the text, as CalcTextSize from the line's start gives it. Asked in increasing order
// (the spans are), each call walks on from the last, so a frame costs the text's length once, not its length for
// every span; asked for an earlier byte, it starts again. A byte inside a UTF-8 character (a non-ASCII byte outside a
// string is a span of its own) is measured from its character's start and not walked past: the next call measures
// that character whole. (In text that is not UTF-8 the sum may stray from CalcTextSize by the odd pixel.)
struct Placer {
    const std::string& text;
    float line_h;
    std::size_t at = 0;
    float line = 0, x = 0; // x: the line's width up to `at`, not yet rounded

    ImVec2 operator()(std::size_t to) {
        to = std::min(to, text.size());
        if (to < at) {
            at = 0;
            line = 0;
            x = 0;
        }
        while (at < to) {
            const std::size_t nl = text.find('\n', at);
            if (nl == std::string::npos || nl >= to) {
                std::size_t whole = to; // the start of the character `to` is in
                while (whole > at && whole < text.size() && (static_cast<unsigned char>(text[whole]) & 0xC0) == 0x80)
                    --whole;
                x += width(at, whole);
                at = whole;
                const float part = whole < to ? width(whole, to) : 0.0f;
                return round(x + part);
            }
            at = nl + 1;
            line += 1;
            x = 0;
        }
        return round(x);
    }

    float width(std::size_t from, std::size_t to) const {
        if (from >= to) return 0.0f;
        return ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, -1.0f, text.data() + from, text.data() + to).x;
    }
    ImVec2 round(float w) const { return ImVec2(w > 0 ? IM_TRUNC(w + 0.99999f) : 0.0f, line * line_h); } // (CalcTextSize's)
};

} // namespace

std::size_t code_editors_kept() { return editors().size(); }

std::vector<std::pair<float, float>> code_editor_places(const std::string& text, const std::vector<std::size_t>& at) {
    Placer place{text, ImGui::GetFontSize()};
    std::vector<std::pair<float, float>> out;
    for (std::size_t to : at) {
        const ImVec2 p = place(to);
        out.emplace_back(p.x, p.y);
    }
    return out;
}

std::int64_t code_editors_oldest() {
    std::int64_t oldest = -1;
    for (const auto& [key, st] : editors())
        if (oldest < 0 || static_cast<std::int64_t>(st.made) < oldest) oldest = static_cast<std::int64_t>(st.made);
    return oldest;
}

namespace {
// The complete function (1) with the text before the cursor (2: a std::string), as CodeEditor's protected call.
int complete_call(lua_State* L) {
    const auto* before = static_cast<const std::string*>(lua_touserdata(L, 2));
    lua_settop(L, 1);
    lua_pushlstring(L, before->data(), before->size());
    lua_call(L, 1, 2);
    return 2;
}
} // namespace

int CodeEditor(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    std::size_t len = 0;
    const char* given = luaL_optlstring(L, 2, "", &len);
    // (Held to float's range: a cast past it is undefined.)
    constexpr double kMax = FLT_MAX;
    float w = static_cast<float>(std::clamp(luaL_optnumber(L, 3, 0), -kMax, kMax));
    float h = static_cast<float>(std::clamp(luaL_optnumber(L, 4, 0), -kMax, kMax));
    int error_line = 0, complete = 0;
    bool read_only = false;
    if (lua_istable(L, 5)) {
        lua_getfield(L, 5, "error_line"); // (a metamethod here may raise: nothing with a destructor exists yet)
        error_line = static_cast<int>(lua_tointeger(L, -1));
        lua_getfield(L, 5, "read_only");
        read_only = lua_toboolean(L, -1) != 0;
        lua_getfield(L, 5, "complete");
        if (lua_isfunction(L, -1)) complete = lua_gettop(L);
        else lua_pop(L, 1);
    }

    std::string text(given, len);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float line_h = ImGui::GetFontSize();
    ImGui::PushID(id);
    const ImGuiID key = ImGui::GetID("##code");
    const std::uint64_t frame = imgui_frame();
    forget_editors(frame);
    EditorState& st = editors()[key];
    st.seen = frame;
    if (st.made == 0) st.made = st.seen;
    const bool showing = !st.names.empty();

    // Ctrl+Up / Ctrl+Down move the highlight (ImGui's box only scrolls on them, and it never scrolls: see above).
    const ImGuiIO& io = ImGui::GetIO();
    if (showing && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_DownArrow))
        st.selected = (st.selected + 1) % static_cast<int>(st.names.size());
    if (showing && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_UpArrow))
        st.selected = (st.selected + static_cast<int>(st.names.size()) - 1) % static_cast<int>(st.names.size());

    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(16, 18, 23, 250)); // a code editor's dark page, opaque
    ImGui::BeginChild("##scroll", ImVec2(w, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PopStyleColor();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float text_w = 0;
    int lines = 1;
    for (std::size_t at = 0;;) {
        std::size_t nl = text.find('\n', at);
        std::size_t end = nl == std::string::npos ? text.size() : nl;
        text_w = std::max(text_w, ImGui::CalcTextSize(text.data() + at, text.data() + end).x);
        if (nl == std::string::npos) break;
        ++lines;
        at = nl + 1;
    }
    ImVec2 size(std::max(avail.x, text_w + style.FramePadding.x * 2 + line_h * 2),
                std::max(avail.y, (static_cast<float>(lines) + 1) * line_h + style.FramePadding.y * 2));

    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    Callback cb{&text, &st, showing};
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize |
                                ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_CallbackAlways;
    if (read_only) flags |= ImGuiInputTextFlags_ReadOnly;
    text.reserve(text.size() + 1);
    bool changed = ImGui::InputTextMultiline("##code", text.data(), text.capacity() + 1, size, flags, callback, &cb);
    text.resize(std::strlen(text.c_str()));
    const bool active = ImGui::IsItemActive();
    st.input = ImGui::GetItemID();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    const ImVec2 origin(ImGui::GetItemRectMin().x + style.FramePadding.x, ImGui::GetItemRectMin().y + style.FramePadding.y);

    // The colours, the error line and the cursor, under the box (its window is a child: drawn after this one).
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (error_line > 0) {
        float y = origin.y + static_cast<float>(error_line - 1) * line_h;
        dl->AddRectFilled(ImVec2(ImGui::GetItemRectMin().x, y), ImVec2(ImGui::GetItemRectMax().x, y + line_h),
                          IM_COL32(160, 40, 40, 90));
    }
    Placer place{text, line_h};
    for (const CodeSpan& s : lua_code_spans(text)) {
        // A span may cross lines (long comments and strings): draw it a line at a time.
        std::size_t at = s.begin;
        while (at < s.end) {
            std::size_t nl = text.find('\n', at);
            std::size_t end = nl == std::string::npos || nl > s.end ? s.end : nl;
            if (end > at) {
                ImVec2 p = place(at);
                dl->AddText(ImVec2(origin.x + p.x, origin.y + p.y), token_color(s.kind), text.data() + at, text.data() + end);
            }
            at = end + 1;
        }
    }
    ImVec2 caret = place(static_cast<std::size_t>(std::max(0, st.cursor)));
    if (active && std::fmod(ImGui::GetTime(), 1.2) < 0.8)
        dl->AddLineV(origin.x + caret.x, origin.y + caret.y, origin.y + caret.y + line_h, IM_COL32(240, 240, 240, 255));

    // Keep the cursor in view when it moved.
    if (active && st.cursor != st.last_cursor) {
        float y = caret.y + style.FramePadding.y, x = caret.x + style.FramePadding.x;
        float sy = ImGui::GetScrollY(), sx = ImGui::GetScrollX();
        float vh = ImGui::GetWindowHeight() - style.ScrollbarSize, vw = ImGui::GetWindowWidth() - style.ScrollbarSize;
        if (y < sy) ImGui::SetScrollY(y);
        else if (y + line_h > sy + vh) ImGui::SetScrollY(y + line_h - vh);
        if (x < sx) ImGui::SetScrollX(std::max(0.0f, x - line_h * 2));
        else if (x > sx + vw - line_h) ImGui::SetScrollX(x - vw + line_h * 2);
    }
    st.last_cursor = st.cursor;
    ImGui::EndChild();

    // The names for the cursor's word, asked when the text before it changed.
    if (!active && !st.hovered) {
        st.names.clear();
        st.asked.clear();
    } else if (active && complete) {
        std::string before = text.substr(0, std::min<std::size_t>(static_cast<std::size_t>(std::max(0, st.cursor)), text.size()));
        if (before != st.asked) {
            st.asked = before;
            st.names.clear();
            st.selected = 0;
            // The text is pushed inside the protected call (complete_call): a push that raised here (the state's
            // memory limit) would skip `text`'s and `before`'s destructors and the ID pushed.
            lua_pushcfunction(L, &complete_call);
            lua_pushvalue(L, complete);
            lua_pushlightuserdata(L, &before);
            if (lua_pcall(L, 2, 2, 0) == LUA_OK) {
                st.start = static_cast<int>(lua_tointeger(L, -2)) - 1;
                if (lua_istable(L, -1)) {
                    // Raw: a __len of the script's could raise here, outside the pcall, past `text` and the ID pushed.
                    const auto n = static_cast<lua_Integer>(std::min<lua_Unsigned>(lua_rawlen(L, -1), 50));
                    for (lua_Integer i = 1; i <= n && i <= 50; ++i) {
                        lua_rawgeti(L, -1, i);
                        if (lua_type(L, -1) == LUA_TSTRING) st.names.emplace_back(lua_tostring(L, -1));
                        lua_pop(L, 1);
                    }
                }
                lua_pop(L, 2);
            } else {
                lua_pop(L, 1);
            }
            // Nothing to offer once the word is written out.
            std::string word = st.start >= 0 && st.start <= static_cast<int>(before.size()) ? before.substr(static_cast<std::size_t>(st.start)) : "";
            if (st.names.size() == 1 && st.names.front() == word) st.names.clear();
            if (st.start < 0 || st.start > static_cast<int>(before.size())) st.names.clear();
        }
    }
    const bool keep = st.hovered;
    st.hovered = false;
    if ((active || keep) && !st.names.empty()) {
        ImGui::SetNextWindowPos(ImVec2(origin.x + caret.x - ImGui::GetScrollX(), origin.y + caret.y + line_h));
        ImGui::Begin("##codenames", nullptr,
                     ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        const int first = std::max(0, st.selected - 11);
        for (int i = first; i < static_cast<int>(st.names.size()) && i < first + 12; ++i) {
            if (ImGui::Selectable(st.names[static_cast<std::size_t>(i)].c_str(), i == st.selected)) {
                // A click: the box is not active now, so the text changes here.
                std::size_t from = static_cast<std::size_t>(st.start), to = static_cast<std::size_t>(st.cursor);
                if (from <= to && to <= text.size()) {
                    text.replace(from, to - from, st.names[static_cast<std::size_t>(i)]);
                    changed = true;
                }
                st.names.clear();
                ImGui::ActivateItemByID(st.input);
            }
        }
        st.hovered = ImGui::IsWindowHovered();
        ImGui::TextDisabled("Tab: take   Ctrl+Up/Down: choose");
        ImGui::End();
    }
    ImGui::PopID();

    // (Moved out of this frame first: a push that raised would skip its destructor.)
    thread_local std::string held;
    held = std::move(text);
    std::string().swap(text);
    lua_pushlstring(L, held.data(), held.size());
    lua_pushboolean(L, changed);
    lua_pushinteger(L, st.cursor);
    lua_pushboolean(L, active);
    return 4;
}

namespace {
// The clipboard holds whatever the player copied (a password too) and is pasted where the player trusts it, so a
// host keeps it from code it does not trust: when the state's registry has a function "hailstone.clipboard_allowed"
// (mq/lua_mq.cpp, a script's state), it is asked first. The UI's own states have none.
void check_clipboard(lua_State* L, const char* what) {
    if (lua_getfield(L, LUA_REGISTRYINDEX, "hailstone.clipboard_allowed") != LUA_TFUNCTION) {
        lua_pop(L, 1);
        return;
    }
    lua_call(L, 0, 1);
    const bool allowed = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    if (!allowed) luaL_error(L, "%s: only a script that may do everything may use the clipboard", what);
}
} // namespace

int SetClipboardText(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    check_clipboard(L, "ui.SetClipboardText");
    ImGui::SetClipboardText(text);
    return 0;
}

int GetClipboardText(lua_State* L) {
    check_clipboard(L, "ui.GetClipboardText");
    const char* t = ImGui::GetClipboardText();
    lua_pushstring(L, t ? t : "");
    return 1;
}

} // namespace hailstone::ui::lua
