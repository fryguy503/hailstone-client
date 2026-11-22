#include "app/chat_tab_bar.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>

#include "app/chat_context_menu.h"
#include "app/chat_tabs.h"
#include "app/lua_ui.h"
#include "ui/lua/lua_host.h"

namespace hailstone::app {

namespace {

namespace tabs = chat_tabs;

ImVec4 rgb(std::uint32_t value) {
    return {static_cast<float>((value >> 16) & 0xff) / 255.0f, static_cast<float>((value >> 8) & 0xff) / 255.0f,
            static_cast<float>(value & 0xff) / 255.0f, 1.0f};
}

std::string_view label(const tabs::Tab& tab) { return tab.title.empty() ? "Chat" : tab.title; }

void draw_menu_rows(const std::vector<UiHost::MenuRow>& rows) {
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        if (row.separator) {
            ImGui::Separator();
            continue;
        }
        ImGui::PushID(static_cast<int>(index));
        if (row.submenu) {
            if (ImGui::BeginMenu(row.text.c_str(), row.enabled)) {
                draw_menu_rows(row.submenu());
                ImGui::EndMenu();
            }
        } else if (ImGui::MenuItem(row.text.c_str(), nullptr, row.check, row.enabled) && row.act) {
            row.act();
        }
        ImGui::PopID();
    }
}

// Read the pack's palette. The strip shares the HUD's theme and window font scale.
ImVec4 palette(lua_State* L, int arg) {
    luaL_checktype(L, arg, LUA_TTABLE);
    float values[] = {1.0f, 1.0f, 1.0f, 1.0f};
    for (int channel = 0; channel < 4; ++channel) {
        lua_rawgeti(L, arg, channel + 1);
        if (const auto value = lua_tonumber(L, -1); lua_isnumber(L, -1) && std::isfinite(value))
            values[channel] = static_cast<float>(std::clamp(value, 0.0, 1.0));
        lua_pop(L, 1);
    }
    return {values[0], values[1], values[2], values[3]};
}

UiHost* modern_chat_host(lua_State* L, int slot) {
    if (!ui::lua::pack_code(L) || slot < 1 || slot > tabs::kMaxSlots || !ImGui::GetCurrentContext()) return nullptr;
    auto name = ui::lua::LuaUi::drawing_window();
    constexpr std::string_view prefix = "Chat ";
    if (!name.starts_with(prefix)) return nullptr;
    name.remove_prefix(prefix.size());
    int window_slot = 0;
    const auto result = std::from_chars(name.data(), name.data() + name.size(), window_slot);
    if (result.ptr != name.data() + name.size() || window_slot != slot) return nullptr;
    if (GImGui->CurrentWindowStack.Size <= 1 || ImGui::GetCurrentWindow()->SkipItems) return nullptr;
    return static_cast<UiHost*>(lua_touserdata(L, lua_upvalueindex(1)));
}

int group_argument(lua_State* L, int arg) {
    const lua_Integer value = luaL_checkinteger(L, arg);
    return value >= 1 && value <= tabs::kMaxSlots ? static_cast<int>(value) : 0;
}

struct ChatTabStyle {
    ImVec4 text, muted, accent, shadow;
};

struct StripItem {
    tabs::Tab tab;
    std::string_view text;
    ImVec2 size;
    bool lit;
};

bool draw_tab_strip(UiHost& host, const tabs::Group& group, ImVec2 pos, float width, float height, float scale,
                    const ChatTabStyle& style, bool rolled) {
    if (group.tabs.empty()) return false;

    ImGui::PushID("chat-tabs");
    ImGui::PushFont(ui::ui_font(3));
    const auto frame_title = group.tabs.size() == 1 ? group.title : std::string_view{};
    std::array<StripItem, tabs::kMaxSlots> items{};
    const auto count = group.tabs.size();
    const float padding = 7.0f * scale;
    float total = 0;
    std::size_t selected = 0;
    const double now = ImGui::GetTime();
    for (std::size_t index = 0; index < count; ++index) {
        auto& item = items[index];
        item.tab = tabs::tab(group.tabs[index]);
        item.text = frame_title.empty() ? label(item.tab) : frame_title;
        item.size = ImGui::CalcTextSize(item.text.data(), item.text.data() + item.text.size());
        item.lit = tabs::highlight_visible(item.tab.slot, now);
        total += item.size.x + 2 * padding;
        if (item.tab.slot == group.selected) selected = index;
    }

    // Always keep the selected tab in view. Overflow lists every tab and reflects a hidden tab's alert.
    const bool overflow = count > 1 && total > width;
    const float more_width = overflow ? std::min(24.0f * scale, width * 0.33f) : 0;
    const float available = width - more_width;
    std::size_t first = overflow ? selected : 0;
    std::size_t last = overflow ? selected + 1 : count;
    float used = items[selected].size.x + 2 * padding;
    if (overflow) {
        while (first > 0 && used + items[first - 1].size.x + 2 * padding <= available)
            used += items[--first].size.x + 2 * padding;
        while (last < count && used + items[last].size.x + 2 * padding <= available)
            used += items[last++].size.x + 2 * padding;
    }

    auto* storage = ImGui::GetStateStorage();
    const ImGuiID popup_tab = ImGui::GetID("tab-popup");
    const ImGuiID popup_serial = ImGui::GetID("tab-popup-serial");
    const auto cursor = ImGui::GetCursorScreenPos();
    float x = pos.x;
    bool took_right_click = false;
    int select = 0;
    for (std::size_t index = first; index < last; ++index) {
        const auto& item = items[index];
        const float end = std::min(x + item.size.x + 2 * padding, pos.x + available);
        if (end - x < 1.0f) break;
        ImGui::PushID(item.tab.slot);
        ImGui::SetCursorScreenPos({x, pos.y + 2 * scale});
        if (ImGui::InvisibleButton("tab", {end - x, std::max(1.0f, height - 2 * scale)}, ImGuiButtonFlags_EnableNav))
            select = item.tab.slot;
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && (index != selected || rolled) && !ImGui::GetIO().KeyAlt &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            took_right_click = true;
            if (host.context_menus()) {
                storage->SetInt(popup_tab, item.tab.slot);
                storage->SetInt(popup_serial, static_cast<int>(item.tab.serial));
            }
        }
        ImGui::PopID();

        const ImVec4 color = item.lit ? rgb(item.tab.highlight_color.value_or(tabs::kDefaultHighlightColor))
                                    : (index == selected || hovered ? style.text : style.muted);
        const ImVec2 text_pos{x + padding, pos.y + (height - item.size.y) * 0.5f};
        auto* draw = ImGui::GetWindowDrawList();
        const float text_end = end - std::min(padding, (end - x) * 0.25f);
        const ImVec4 clip{x, pos.y, text_end, pos.y + height};
        const auto* begin = item.text.data();
        const auto* finish = begin + item.text.size();
        draw->AddText(nullptr, 0, {text_pos.x + 1, text_pos.y + 1}, ImGui::GetColorU32(style.shadow), begin, finish, 0, &clip);
        draw->AddText(nullptr, 0, text_pos, ImGui::GetColorU32(color), begin, finish, 0, &clip);
        if (index == selected && count > 1 && text_end > text_pos.x)
            draw->AddLine({text_pos.x, pos.y + height - 3 * scale}, {text_end, pos.y + height - 3 * scale},
                          ImGui::GetColorU32(style.accent), 2 * scale);
        if (hovered) {
            const auto tip = item.size.x + 2 * padding > end - x || item.text != label(item.tab) ? label(item.tab)
                : index == selected && item.text != group.title ? group.title : std::string_view{};
            if (!tip.empty()) ImGui::SetTooltip("%.*s", static_cast<int>(tip.size()), tip.data());
        }
        x = end;
    }
    if (took_right_click && host.context_menus()) ImGui::OpenPopup("tab-context");

    if (overflow && more_width >= 1.0f) {
        ImVec4 color = style.muted;
        for (std::size_t index = 0; index < count; ++index) {
            if (index >= first && index < last) continue;
            if (items[index].lit) {
                color = rgb(items[index].tab.highlight_color.value_or(tabs::kDefaultHighlightColor));
                break;
            }
        }
        const float left = pos.x + available;
        ImGui::SetCursorScreenPos({left, pos.y + 2 * scale});
        if (ImGui::InvisibleButton("more", {more_width, std::max(1.0f, height - 2 * scale)}, ImGuiButtonFlags_EnableNav))
            ImGui::OpenPopup("all-tabs");
        const float cx = left + more_width * 0.5f, cy = pos.y + height * 0.5f;
        const float arrow = std::min(4.0f * scale, more_width * 0.25f);
        ImGui::GetWindowDrawList()->AddTriangleFilled({cx - arrow, cy - arrow * 0.5f},
            {cx + arrow, cy - arrow * 0.5f}, {cx, cy + arrow * 0.5f}, ImGui::GetColorU32(color));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("All chat tabs");
    }
    if (ImGui::BeginPopup("all-tabs")) {
        for (const auto& item : std::span(items).first(count)) {
            const auto text = std::string(label(item.tab));
            ImGui::PushID(item.tab.slot);
            if (item.lit) ImGui::PushStyleColor(ImGuiCol_Text, rgb(item.tab.highlight_color.value_or(tabs::kDefaultHighlightColor)));
            if (ImGui::MenuItem(text.c_str(), nullptr, item.tab.slot == group.selected)) select = item.tab.slot;
            if (item.lit) ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    if (select) tabs::select(group.slot, select);
    if (ImGui::BeginPopup("tab-context")) {
        const auto target = tabs::tab(storage->GetInt(popup_tab));
        if (host.context_menus() && target.slot && tabs::group_of(target.slot) == group.slot &&
            static_cast<int>(target.serial) == storage->GetInt(popup_serial))
            draw_menu_rows(chat_context_rows(target.slot));
        else
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SetCursorScreenPos(cursor);
    ImGui::PopFont();
    ImGui::PopID();
    return took_right_click;
}

int ChatTabStrip(lua_State* L) {
    const int slot = group_argument(L, 1);
    auto* host = modern_chat_host(L, slot);
    if (!host) return 0;
    float bounds[5]{};
    for (int i = 0; i < 5; ++i) bounds[i] = static_cast<float>(luaL_checknumber(L, i + 2));
    if (std::any_of(std::begin(bounds), std::end(bounds), [](float v) { return !std::isfinite(v); }) ||
        bounds[2] < 1 || bounds[3] < 1 || bounds[4] <= 0) return 0;
    const ChatTabStyle style{palette(L, 7), palette(L, 8), palette(L, 9), palette(L, 10)};
    lua_pushboolean(L, draw_tab_strip(*host, tabs::group(slot), {bounds[0], bounds[1]}, bounds[2], bounds[3],
                                     bounds[4], style, lua_toboolean(L, 11)));
    return 1;
}

int ChatTabMenu(lua_State* L) {
    const int slot = group_argument(L, 1);
    auto* host = modern_chat_host(L, slot);
    if (!host || !host->context_menus()) return 0;
    const auto group = tabs::group(slot);
    if (group.selected) draw_menu_rows(chat_context_rows(group.selected));
    return 0;
}

int ScrollChatTab(lua_State* L) {
    const int slot = group_argument(L, 1);
    const int tab_slot = group_argument(L, 2);
    if (!modern_chat_host(L, slot) || !tab_slot ||
        !(ImGui::GetCurrentWindow()->Flags & ImGuiWindowFlags_ChildWindow)) return 0;
    const auto tab = tabs::tab(tab_slot);
    if (!tab.slot) return 0;

    struct Viewport {
        std::uint32_t serial = 0;
        int frame = -1;
        int group = 0;
        float y = 0;
        bool follow = true;
    };
    static std::array<Viewport, tabs::kMaxSlots> viewports;
    auto& view = viewports[static_cast<std::size_t>(tab_slot - 1)];
    if (view.serial != tab.serial) view = Viewport{tab.serial};
    const int frame = ImGui::GetFrameCount();
    const bool returning = view.group != slot || view.frame + 1 != frame;
    const float y = ImGui::GetScrollY();
    const bool at_bottom = y >= ImGui::GetScrollMaxY() - 2;
    const bool follow = lua_toboolean(L, 5);
    if (lua_isnumber(L, 3)) {
        const float delta = static_cast<float>(lua_tonumber(L, 3) * luaL_checknumber(L, 4));
        if (std::isfinite(delta)) ImGui::SetScrollY(std::max(0.0f, (returning ? view.y : y) + delta));
    } else if (follow || (returning ? view.follow : at_bottom)) {
        ImGui::SetScrollHereY(1.0f);
    } else if (returning) {
        ImGui::SetScrollY(view.y);
    }
    if (!returning) {
        view.y = y;
        view.follow = at_bottom;
    }
    view.group = slot;
    view.frame = frame;
    return 0;
}

} // namespace

void register_modern_chat_tabs(lua_State* L, UiHost& host) {
    lua_getglobal(L, "ui");
    if (lua_istable(L, -1)) {
        for (const luaL_Reg binding : {luaL_Reg{"ChatTabStrip", ChatTabStrip}, luaL_Reg{"ChatTabMenu", ChatTabMenu},
                                      luaL_Reg{"ScrollChatTab", ScrollChatTab}}) {
            lua_pushlightuserdata(L, &host);
            lua_pushcclosure(L, binding.func, 1);
            lua_setfield(L, -2, binding.name);
        }
    }
    lua_pop(L, 1);
}

void position_detached_chat(UiHost& host, int source, int destination) {
    if (!host.layout_setting || !host.set_layout_setting || !GImGui) return;
    const auto from = std::format("Chat{}", source);
    const auto to = std::format("Chat{}", destination);
    const auto source_window = std::format("###modern.{}", from);
    const auto* frame = ImGui::FindWindowByName(source_window.c_str());
    if (!frame || frame->LastFrameActive < ImGui::GetFrameCount() - 1) return;

    for (const std::string_view key : {"scale", "lock", "size"}) {
        const auto original = host.layout_setting(std::format("hud.{}.{}", key, from));
        host.set_layout_setting(std::format("hud.{}.{}", key, to), original);
    }
    const float scale = ui::ui_scale() * frame->FontWindowScale;
    if (scale > 0 && host.layout_setting("hud.rolled." + from) != "1" && frame->Size.y > 32) {
        host.set_layout_setting("hud.size." + to, std::format("{:.1f},{:.1f}", frame->Size.x / scale, frame->Size.y / scale));
    }
    host.set_layout_setting("hud.rolled." + to, "");
    host.set_layout_setting("hud.pos." + to, std::format("{:.0f},{:.0f}", frame->Pos.x + 24, frame->Pos.y + 24));
}

} // namespace hailstone::app
