// Offline visual fixture for the actual nameplate painter. No game files, connection or GPU are used.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <imgui.h>

#include "app/nameplate_layer.h"
#include "core/png_write.h"

namespace {

constexpr int kWidth = 1600, kHeight = 940;

float edge(ImVec2 a, ImVec2 b, ImVec2 p) { return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x); }

std::vector<std::byte> raster(const ImDrawData& data, const unsigned char* atlas, int atlas_width, int atlas_height) {
    std::vector<std::byte> rgba(static_cast<std::size_t>(kWidth) * kHeight * 4);
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = std::byte{13}; rgba[i + 1] = std::byte{18}; rgba[i + 2] = std::byte{28}; rgba[i + 3] = std::byte{255};
    }
    const auto sample = [&](float u, float v) {
        const float x = std::clamp(u * static_cast<float>(atlas_width) - 0.5f, 0.0f, static_cast<float>(atlas_width - 1));
        const float y = std::clamp(v * static_cast<float>(atlas_height) - 0.5f, 0.0f, static_cast<float>(atlas_height - 1));
        const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
        const int x1 = std::min(x0 + 1, atlas_width - 1), y1 = std::min(y0 + 1, atlas_height - 1);
        const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
        const auto alpha = [&](int px, int py) { return static_cast<float>(atlas[(py * atlas_width + px) * 4 + 3]) / 255.0f; };
        return (alpha(x0, y0) * (1 - fx) + alpha(x1, y0) * fx) * (1 - fy) +
               (alpha(x0, y1) * (1 - fx) + alpha(x1, y1) * fx) * fy;
    };
    for (int list = 0; list < data.CmdListsCount; ++list) {
        const auto& dl = *data.CmdLists[list];
        for (const ImDrawCmd& cmd : dl.CmdBuffer) {
            if (cmd.UserCallback) continue;
            for (unsigned int n = 0; n + 2 < cmd.ElemCount; n += 3) {
                const ImDrawVert& a = dl.VtxBuffer[static_cast<int>(cmd.VtxOffset + dl.IdxBuffer[static_cast<int>(cmd.IdxOffset + n)])];
                const ImDrawVert& b = dl.VtxBuffer[static_cast<int>(cmd.VtxOffset + dl.IdxBuffer[static_cast<int>(cmd.IdxOffset + n + 1)])];
                const ImDrawVert& c = dl.VtxBuffer[static_cast<int>(cmd.VtxOffset + dl.IdxBuffer[static_cast<int>(cmd.IdxOffset + n + 2)])];
                const float area = edge(a.pos, b.pos, c.pos);
                if (std::abs(area) < 0.00001f) continue;
                const int x0 = std::max(0, static_cast<int>(std::floor(std::max(cmd.ClipRect.x, std::min({a.pos.x, b.pos.x, c.pos.x})))));
                const int y0 = std::max(0, static_cast<int>(std::floor(std::max(cmd.ClipRect.y, std::min({a.pos.y, b.pos.y, c.pos.y})))));
                const int x1 = std::min(kWidth - 1, static_cast<int>(std::ceil(std::min(cmd.ClipRect.z, std::max({a.pos.x, b.pos.x, c.pos.x})))));
                const int y1 = std::min(kHeight - 1, static_cast<int>(std::ceil(std::min(cmd.ClipRect.w, std::max({a.pos.y, b.pos.y, c.pos.y})))));
                for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
                    const ImVec2 p(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
                    const float wa = edge(b.pos, c.pos, p) / area, wb = edge(c.pos, a.pos, p) / area, wc = 1.0f - wa - wb;
                    if (wa < 0 || wb < 0 || wc < 0) continue;
                    const auto channel = [&](int shift) {
                        return static_cast<float>((a.col >> shift) & 255) * wa + static_cast<float>((b.col >> shift) & 255) * wb +
                               static_cast<float>((c.col >> shift) & 255) * wc;
                    };
                    const float alpha = channel(IM_COL32_A_SHIFT) / 255.0f * sample(a.uv.x * wa + b.uv.x * wb + c.uv.x * wc,
                                                                                  a.uv.y * wa + b.uv.y * wb + c.uv.y * wc);
                    const std::size_t at = (static_cast<std::size_t>(y) * kWidth + static_cast<std::size_t>(x)) * 4;
                    constexpr std::array<int, 3> shifts = {IM_COL32_R_SHIFT, IM_COL32_G_SHIFT, IM_COL32_B_SHIFT};
                    for (std::size_t ch = 0; ch < shifts.size(); ++ch) {
                        const float old = static_cast<float>(std::to_integer<unsigned char>(rgba[at + ch]));
                        rgba[at + ch] = static_cast<std::byte>(static_cast<unsigned char>(std::clamp(channel(shifts[ch]) * alpha + old * (1 - alpha), 0.0f, 255.0f)));
                    }
                }
            }
        }
    }
    return rgba;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) { std::cerr << "Usage: hailstone_plate_preview output.png [font.ttf]\n"; return 1; }
    if (argc == 3 && !std::filesystem::is_regular_file(argv[2])) { std::cerr << "Font file not found\n"; return 1; }
    ImGuiContext* context = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(static_cast<float>(kWidth), static_cast<float>(kHeight));
    io.DeltaTime = 1.0f / 60.0f;
    if (argc == 3) io.Fonts->AddFontFromFileTTF(argv[2], 18.0f);
    unsigned char* atlas = nullptr;
    int atlas_width = 0, atlas_height = 0;
    io.Fonts->GetTexDataAsRGBA32(&atlas, &atlas_width, &atlas_height);
    ImGui::NewFrame();
    ImDrawList& dl = *ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const auto label = [&](float x, float y, const char* text, float size, ImU32 color) {
        dl.AddText(font, size, ImVec2(x, y), color, text);
    };
    label(44, 28, "HAILSTONE CREST", 32, IM_COL32(235, 242, 255, 255));
    label(44, 76, "Offline renderer preview | Shipped presets with shared input and colors", 16, IM_COL32(146, 163, 185, 255));
    label(44, 102, "Health 72% | Mana 36% | Cast 55% | Preset sizing and visibility preserved", 14, IM_COL32(146, 163, 185, 255));
    const auto card = [&](float x, float y, const char* title, const char* description) {
        dl.AddRectFilled(ImVec2(x, y), ImVec2(x + 360, y + 340), IM_COL32(22, 30, 44, 255), 12);
        dl.AddRect(ImVec2(x, y), ImVec2(x + 360, y + 340), IM_COL32(49, 64, 85, 255), 12);
        label(x + 22, y + 22, title, 18, IM_COL32(132, 192, 248, 255));
        label(x + 22, y + 298, description, 13, IM_COL32(146, 163, 185, 255));
    };
    using hailstone::app::NameplateText;
    using hailstone::game::NameplateBar;
    constexpr std::array<const char*, 8> names{{"DEFAULT", "BARE", "INSET", "FORGED", "GLASS", "RUNIC", "SPLIT", "TACTICAL"}};
    constexpr std::array<const char*, 8> descriptions{{
        "Solid frame | grouped resources | arrow",
        "Below name | side marks | detached cast",
        "Square inset | chevrons | no resources",
        "Gloss bevel | side marks | brackets",
        "Inset glass | lower marks | detached cast",
        "Angular segments | side sigils | chevrons",
        "Thin strip | narrow cast | side indicators",
        "Inset segments | lower marks | brackets"}};
    for (std::size_t i = 0; i < names.size(); ++i) {
        const float x = 44.0f + static_cast<float>(i % 4) * 384.0f;
        const float y = i < 4 ? 132.0f : 496.0f;
        card(x, y, names[i], descriptions[i]);
        NameplateText p;
        p.pos = {x + 180.0f, y + 100.0f};
        p.target = true;
        p.name = "Obsidian Warden";
        p.presentation.emplace();
        auto& a = *p.presentation;
        // Structural mappings mirror the shipped Crest config.lua presets; only colors and unit data are shared.
        a.width = 142; a.height = 12; a.cast_height = 9; a.mana_height = 4;
        a.scale = 1.15f; // the shipped target-scale multiplier
        a.subtitle = "Lv 65";
        a.texture = "gradient"; a.border_color = 0xd5b36a; a.background_color = 0x111821;
        a.health = NameplateBar{72, 0xd95c64, "72%"};
        a.mana = NameplateBar{36, 0x48a1de, ""};
        a.cast = NameplateBar{55, 0xd6b057, "Obsidian Flame 1.4s"};
        a.badges = {{"star", "1", 0xd5b36a}, {"sleep", "", 0xd5b36a}, {"circle", "", 0xd5b36a}};
        hailstone::app::NameplateDrawStyle style;
        style.size = 18;
        switch (i) {
        case 0:
            break;
        case 1:
            a.width = 110; a.height = 8; a.font_scale = 0.85f; a.subtitle.reset(); a.mana.reset();
            a.frame_style = "minimal"; a.texture = "flat"; a.border_width = 0; a.rounding = 0;
            a.name_position = "below"; a.target_style = "brackets"; a.badge_position = "right"; a.badge_scale = 0.8f;
            a.health_text_align = "right"; a.cast_gap = 7; a.cast_width = 92;
            break;
        case 2:
            a.width = 154; a.height = 18; a.frame_style = "solid"; a.texture = "flat"; a.name_position = "inside";
            a.text_align = "left"; a.health_text_align = "right"; a.target_style = "chevrons"; a.subtitle.reset();
            a.rounding = 0; a.border_width = 2; a.mana.reset(); a.badge_position = "above";
            break;
        case 3:
            a.width = 172; a.height = 16; a.frame_style = "bevel"; a.texture = "gloss"; a.target_style = "brackets";
            a.mana_height = 8;
            a.rounding = 3; a.border_width = 3; a.name_position = "above";
            a.badge_position = "left"; a.badge_scale = 1.1f; a.health_text_align = "right";
            a.cast_gap = 5; a.cast_height = 15; a.cast_text_align = "center";
            break;
        case 4:
            a.width = 174; a.height = 19; a.frame_style = "glass"; a.texture = "gloss"; a.target_style = "glow";
            a.rounding = 7; a.border_width = 1; a.name_position = "inside"; a.text_align = "left"; a.health_text_align = "right";
            a.badge_position = "below"; a.cast_gap = 10; a.cast_width = 142; a.cast_height = 12; a.subtitle.reset();
            a.alpha = 0.88f;
            break;
        case 5:
            a.width = 184; a.height = 19; a.frame_style = "runic"; a.texture = "segments"; a.target_style = "chevrons";
            a.mana_height = 6;
            a.rounding = 0; a.border_width = 2; a.name_position = "above"; a.font_scale = 1.15f;
            a.badge_position = "left"; a.badge_scale = 1.25f; a.cast_gap = 8; a.cast_width = 146; a.cast_height = 15;
            break;
        case 6:
            a.width = 176; a.height = 11; a.frame_style = "minimal"; a.texture = "flat"; a.target_style = "brackets";
            a.name_position = "below"; a.subtitle.reset(); a.cast_gap = 18; a.cast_width = 124; a.cast_height = 14;
            a.badge_position = "right"; a.badge_scale = 1.25f; a.health_text_align = "right"; a.cast_text_align = "left";
            a.border_width = 1; a.rounding = 0; a.mana->text = "36%";
            break;
        case 7:
            a.width = 166; a.height = 13; a.frame_style = "solid"; a.texture = "segments"; a.target_style = "brackets";
            a.name_position = "inside"; a.text_align = "left"; a.health_text_align = "right"; a.subtitle.reset();
            a.badge_position = "below"; a.cast_gap = 6; a.cast_width = 166; a.cast_height = 11; a.rounding = 0;
            break;
        default: break;
        }
        const auto size = hailstone::app::nameplate_size(p, style);
        p.pos.y = y + 76.0f + std::max(0.0f, (190.0f - size.y) * 0.5f);
        hailstone::app::paint_nameplate(dl, p, style);
    }
    label(44, 892, "Synthetic examples. Live gameplay and server testing remain pending.", 14, IM_COL32(146, 163, 185, 255));
    ImGui::Render();
    const auto pixels = raster(*ImGui::GetDrawData(), atlas, atlas_width, atlas_height);
    const bool ok = hailstone::write_png_rgba(argv[1], pixels, kWidth, kHeight);
    ImGui::DestroyContext(context);
    if (!ok) { std::cerr << "Could not write preview\n"; return 1; }
    std::cout << argv[1] << '\n';
    return 0;
}
