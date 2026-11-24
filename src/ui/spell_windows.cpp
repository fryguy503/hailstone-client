#include "ui/spell_windows.h"

#include <algorithm>
#include <format>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include "assets/texture.h"
#include "core/game_dir.h"
#include "core/log.h"
#include "render/gpu.h"
#include "render/vk_context.h"
#include "render/vk_util.h"
#include "ui/imgui_vk_textures.h"

namespace hailstone::ui {

// ---------------------------------------------------------------- icons

std::optional<SpellIconCell> spell_icon_cell(std::int32_t icon) {
    if (icon < 0) return std::nullopt;
    SpellIconCell c;
    c.sheet = icon / 36 + 1;
    if (c.sheet > 7) return std::nullopt; // spells01..spells07.tga
    int cell = icon % 36;
    // Horizontal grid (<Vertical>false</Vertical>): across each row first.
    c.x = (cell % 6) * 40;
    c.y = (cell / 6) * 40;
    return c;
}

struct SpellIconCache::Sheet {
    render::Texture texture;
    VkDescriptorSet set = VK_NULL_HANDLE;
    float width = 256.0f, height = 256.0f;
};

SpellIconCache::SpellIconCache(render::VkContext& vk, const GameDir& game) : vk_(vk), game_(game) {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f; // the base level only, so the cells do not bleed into each other
    VkSampler sampler = VK_NULL_HANDLE;
    render::vk_check(vkCreateSampler(vk_.device(), &si, nullptr, &sampler), "vkCreateSampler (spell icons)");
    sampler_ = sampler;
}

SpellIconCache::~SpellIconCache() {
    vk_.wait_idle();
    for (auto& [n, s] : sheets_) {
        if (!s) continue;
        if (s->set) ui::remove_imgui_texture(s->set);
        s->texture.destroy(vk_.device(), vk_.allocator());
    }
    if (sampler_) vkDestroySampler(vk_.device(), static_cast<VkSampler>(sampler_), nullptr);
}

const SpellIconCache::Sheet* SpellIconCache::sheet(int number) {
    auto it = sheets_.find(number);
    if (it != sheets_.end()) return it->second.get();
    std::unique_ptr<Sheet> s;
    std::string name = std::format("uifiles/default/spells{:02}.tga", number);
    if (auto data = game_.read(name)) {
        std::string error;
        if (auto image = assets::decode_texture(*data, {}, &error)) {
            assets::Image rgba = assets::to_rgba8(*image);
            rgba.mips.resize(1);
            s = std::make_unique<Sheet>();
            s->width = static_cast<float>(rgba.width);
            s->height = static_cast<float>(rgba.height);
            render::UploadBatch batch(vk_);
            s->texture = batch.texture(rgba);
            batch.submit();
            s->set = ui::add_imgui_texture(s->texture.view,
                                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (!s->set) {
                log::warn("spell icons: no descriptor left for {}", name);
                // Its upload may still be on the GPU.
                vk_.retire([texture = s->texture, device = vk_.device(), allocator = vk_.allocator()]() mutable {
                    texture.destroy(device, allocator);
                });
                s.reset();
            }
        } else {
            log::warn("spell icons: {}: {}", name, error);
        }
    }
    return (sheets_[number] = std::move(s)).get();
}

bool SpellIconCache::draw_at(void* draw_list, std::int32_t icon, float x, float y, float size, std::uint32_t tint) {
    auto cell = spell_icon_cell(icon);
    const Sheet* s = cell ? sheet(cell->sheet) : nullptr;
    auto* dl = static_cast<ImDrawList*>(draw_list);
    if (!s) {
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + size, y + size), IM_COL32(60, 60, 90, 230), 3.0f);
        return false;
    }
    ImVec2 uv0(static_cast<float>(cell->x) / s->width, static_cast<float>(cell->y) / s->height);
    ImVec2 uv1(static_cast<float>(cell->x + 40) / s->width, static_cast<float>(cell->y + 40) / s->height);
    dl->AddImage(reinterpret_cast<ImTextureID>(s->set), ImVec2(x, y), ImVec2(x + size, y + size), uv0, uv1, tint);
    return true;
}

namespace {

constexpr ImGuiWindowFlags kPanelFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;

// An icon button: returns 1 on a left click, 2 on a right click.
int icon_button(const char* id, std::int32_t icon, float size, SpellIconCache* icons, std::uint32_t tint,
                const char* fallback) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(size, size), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    int clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left) ? 1 : ImGui::IsItemClicked(ImGuiMouseButton_Right) ? 2 : 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool drawn = icon >= 0 && icons && icons->draw_at(dl, icon, p.x, p.y, size, tint);
    if (!drawn) {
        dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), icon >= 0 ? IM_COL32(70, 70, 110, 255) : IM_COL32(35, 35, 40, 255), 3.0f);
        if (fallback && *fallback) {
            char initial[2] = {fallback[0], 0};
            ImVec2 ts = ImGui::CalcTextSize(initial);
            dl->AddText(ImVec2(p.x + (size - ts.x) * 0.5f, p.y + (size - ts.y) * 0.5f), IM_COL32(230, 230, 230, 255), initial);
        }
    }
    if (ImGui::IsItemHovered()) dl->AddRect(p, ImVec2(p.x + size, p.y + size), IM_COL32(255, 255, 255, 160), 3.0f);
    return clicked;
}

void buff_rows(const std::vector<BuffView>& buffs, SpellActions* actions, SpellIconCache* icons, const char* prefix) {
    float size = ImGui::GetTextLineHeight() * 1.4f;
    for (const auto& b : buffs) {
        ImGui::PushID(b.slot);
        int c = icon_button(std::format("##{}{}", prefix, b.slot).c_str(), b.icon, size, icons, 0xffffffff, b.name.c_str());
        if (ImGui::IsItemHovered() && !b.tooltip.empty()) ImGui::SetTooltip("%s", b.tooltip.c_str());
        if (c == 2 && actions && b.beneficial) actions->remove_buff = b.slot;
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextColored(b.beneficial ? ImVec4(0.75f, 0.85f, 1.0f, 1.0f) : ImVec4(1.0f, 0.6f, 0.5f, 1.0f), "%s", b.name.c_str());
        ImGui::TextDisabled("%s", b.time_left.c_str());
        ImGui::EndGroup();
        ImGui::PopID();
    }
}

} // namespace

// ---------------------------------------------------------------- windows

void spell_bar_window(const SpellBarView& view, SpellActions& actions, SpellIconCache* icons) {
    float em = ImGui::GetFontSize();
    float size = em * 2.2f;
    ImVec2 display = ImGui::GetIO().DisplaySize;
    // A row of gems in the bottom-right corner, clear of the chat window.
    ImGui::SetNextWindowPos(ImVec2(display.x - em * 0.6f, display.y - em * 0.6f), ImGuiCond_FirstUseEver, ImVec2(1.0f, 1.0f));
    ImGui::Begin("Spell Gems##hailstone_gems", nullptr, kPanelFlags | ImGuiWindowFlags_AlwaysAutoResize);
    for (std::size_t i = 0; i < view.gems.size(); ++i) {
        const auto& g = view.gems[i];
        if (i) ImGui::SameLine();
        ImGui::PushID(static_cast<int>(i));
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool empty = g.spell == 0xffffffff;
        std::uint32_t tint = !g.usable ? IM_COL32(90, 90, 90, 255) : IM_COL32(255, 255, 255, 255);
        int c = icon_button("##gem", empty ? -1 : g.icon, size, icons, tint, empty ? "" : g.name.c_str());
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (!empty && g.refresh > 0.0f) {
            // Not ready: darken the part still refreshing (RefreshSpellGemButtons tints the gem).
            dl->AddRectFilled(ImVec2(p.x, p.y + size * (1.0f - g.refresh)), ImVec2(p.x + size, p.y + size),
                              IM_COL32(0, 0, 0, 150));
        }
        if (g.casting) dl->AddRect(p, ImVec2(p.x + size, p.y + size), IM_COL32(255, 220, 80, 255), 3.0f, 2.0f, 0);
        if (g.memorize >= 0.0f) {
            dl->AddRectFilled(ImVec2(p.x, p.y + size - 4.0f), ImVec2(p.x + size * g.memorize, p.y + size),
                              IM_COL32(120, 200, 255, 255));
        }
        if (!g.usable) dl->AddText(ImVec2(p.x + 2, p.y + 2), IM_COL32(200, 200, 200, 200), "x");
        std::string num = std::to_string(i + 1);
        dl->AddText(ImVec2(p.x + size - ImGui::CalcTextSize(num.c_str()).x - 2, p.y + 1), IM_COL32(255, 255, 255, 200), num.c_str());
        if (ImGui::IsItemHovered()) {
            if (!empty) ImGui::SetTooltip("%s", g.tooltip.c_str());
            else ImGui::SetTooltip("Gem %zu: empty%s", i + 1, g.usable ? " (memorize from the spell book)" : " (locked)");
        }
        if (c == 1 && !empty) actions.cast_gem = static_cast<int>(i);
        if (c == 2 && !empty) actions.forget_gem = static_cast<int>(i);
        ImGui::PopID();
    }
    if (view.show_book_button) {
        ImGui::SameLine();
        if (ImGui::Button("Book", ImVec2(0, size))) actions.toggle_book = true;
    }
    ImGui::End();
}

void cast_bar_window(const CastBarView& view) {
    if (!view.visible && view.memorize_fraction < 0.0f) return;
    float em = ImGui::GetFontSize();
    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.72f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(em * 18.0f, 0.0f));
    ImGui::Begin("##hailstone_castbar", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize);
    if (view.visible) {
        ImGui::TextUnformatted(view.spell.c_str());
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.85f, 0.55f, 0.95f, 1.0f));
        ImGui::ProgressBar(view.fraction, ImVec2(-1.0f, em * 0.9f), std::format("{:.1f}s", view.seconds_left).c_str());
        ImGui::PopStyleColor();
    }
    if (view.memorize_fraction >= 0.0f) {
        ImGui::TextUnformatted(view.memorizing.c_str());
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.45f, 0.75f, 1.0f, 1.0f));
        ImGui::ProgressBar(view.memorize_fraction, ImVec2(-1.0f, em * 0.9f), "");
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

void spell_book_window(SpellBookView& view, SpellActions& actions, SpellIconCache* icons) {
    if (!view.open) return;
    float em = ImGui::GetFontSize();
    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.45f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(em * 36.0f, em * 26.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Spell Book##hailstone_book", &view.open, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    constexpr int kPerPage = 16; // a spread: two pages of 8 (CSpellBookWnd)
    int pages = std::max(1, (720 + kPerPage - 1) / kPerPage);
    // Only spreads that hold spells are worth paging through: jump between them.
    int last_used = 0;
    for (const auto& e : view.entries) last_used = std::max(last_used, e.slot / kPerPage);
    view.page = std::clamp(view.page, 0, std::min(pages - 1, last_used));
    if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) view.page = std::max(0, view.page - 1);
    ImGui::SameLine();
    ImGui::Text("Pages %d-%d", view.page * 2 + 1, view.page * 2 + 2);
    ImGui::SameLine();
    if (ImGui::ArrowButton("##next", ImGuiDir_Right)) view.page = std::min(last_used, view.page + 1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 7.0f);
    std::string gem_label = view.target_gem < 0 ? "first empty" : std::format("gem {}", view.target_gem + 1);
    if (ImGui::BeginCombo("Memorize into", gem_label.c_str())) {
        if (ImGui::Selectable("first empty", view.target_gem < 0)) view.target_gem = -1;
        for (int g = 0; g < view.usable_gems; ++g)
            if (ImGui::Selectable(std::format("gem {}", g + 1).c_str(), view.target_gem == g)) view.target_gem = g;
        ImGui::EndCombo();
    }
    ImGui::Separator();
    float size = em * 2.2f;
    if (ImGui::BeginTable("##book", 2, ImGuiTableFlags_SizingStretchSame)) {
        for (int half = 0; half < 2; ++half) {
            ImGui::TableNextColumn();
            for (int i = 0; i < 8; ++i) {
                int slot = view.page * kPerPage + half * 8 + i;
                auto it = std::find_if(view.entries.begin(), view.entries.end(), [&](const auto& e) { return e.slot == slot; });
                ImGui::PushID(slot);
                if (it == view.entries.end()) {
                    if (view.cursor_scroll) {
                        if (icon_button("##empty", -1, size, icons, IM_COL32(255, 255, 255, 255), "+") == 1) actions.scribe = slot;
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scribe the scroll on the cursor here");
                    } else {
                        ImGui::Dummy(ImVec2(size, size));
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%d", slot + 1);
                } else {
                    int c = icon_button("##spell", it->icon, size, icons, IM_COL32(255, 255, 255, 255), it->name.c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it->tooltip.c_str());
                    if (c == 1 && view.cursor_scroll) actions.scribe = slot;
                    else if (c == 1) actions.memorize = std::make_pair(slot, view.target_gem);
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    if (it->memorized) ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "%s", it->name.c_str());
                    else ImGui::TextUnformatted(it->name.c_str());
                    ImGui::TextDisabled("Level %d", it->level);
                    ImGui::EndGroup();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    if (!view.status.empty()) ImGui::TextDisabled("%s", view.status.c_str());
    else if (view.cursor_scroll) ImGui::TextDisabled("Click an empty slot to scribe the scroll on the cursor.");
    else ImGui::TextDisabled("Click a spell to memorize it; right-click a gem to forget it.");
    ImGui::End();
}

void buff_windows(const BuffWindowView& view, SpellActions& actions, SpellIconCache* icons) {
    float em = ImGui::GetFontSize();
    ImVec2 display = ImGui::GetIO().DisplaySize;
    if (!view.buffs.empty()) {
        ImGui::SetNextWindowPos(ImVec2(display.x - em * 14.0f, display.y * 0.48f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(em * 13.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Buffs##hailstone_buffs", nullptr, kPanelFlags);
        buff_rows(view.buffs, &actions, icons, "b");
        ImGui::End();
    }
    if (!view.songs.empty()) {
        ImGui::SetNextWindowPos(ImVec2(display.x - em * 28.0f, display.y * 0.48f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(em * 13.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Songs##hailstone_songs", nullptr, kPanelFlags);
        buff_rows(view.songs, &actions, icons, "s");
        ImGui::End();
    }
    if (!view.target_name.empty() && !view.target.empty()) {
        ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f + em * 12.0f, em * 0.5f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(em * 13.0f, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin(std::format("{}'s effects###hailstone_tbuffs", view.target_name).c_str(), nullptr, kPanelFlags);
        buff_rows(view.target, nullptr, icons, "t");
        ImGui::End();
    }
}

} // namespace hailstone::ui
