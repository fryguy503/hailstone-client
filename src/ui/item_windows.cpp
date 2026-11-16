#include "ui/item_windows.h"
#include "ui/lua/lua_host.h"

#include <algorithm>
#include <cstdio>
#include <format>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include "assets/texture.h"
#include "core/game_dir.h"
#include "core/log.h"
#include "game/eq_names.h"
#include "render/gpu.h"
#include "render/vk_context.h"
#include "render/vk_util.h"
#include "ui/imgui_vk_textures.h"

namespace hailstone::ui {

using net::Item;
using net::ItemSlot;

// ---------------------------------------------------------------- icons

std::optional<IconCell> icon_cell(std::uint32_t icon) {
    if (icon < 500) return std::nullopt;
    std::uint32_t index = icon - 500;
    std::uint32_t cell = index % 36;
    IconCell c;
    c.sheet = static_cast<int>(index / 36) + 1;
    // Vertical grid: cells run down each 6-cell column first.
    c.x = static_cast<int>(cell / 6) * 40;
    c.y = static_cast<int>(cell % 6) * 40;
    return c;
}

struct ItemIconCache::Sheet {
    render::Texture texture;
    VkDescriptorSet set = VK_NULL_HANDLE;
    float width = 256.0f, height = 256.0f;
};

ItemIconCache::ItemIconCache(render::VkContext& vk, const GameDir& game) : vk_(vk), game_(game) {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f; // the base level only: the 40-pixel cells must not bleed into each other
    VkSampler sampler = VK_NULL_HANDLE;
    render::vk_check(vkCreateSampler(vk_.device(), &si, nullptr, &sampler), "vkCreateSampler (item icons)");
    sampler_ = sampler;
}

ItemIconCache::~ItemIconCache() {
    vk_.wait_idle();
    for (auto& [n, s] : sheets_) {
        if (!s) continue;
        if (s->set) ui::remove_imgui_texture(s->set);
        s->texture.destroy(vk_.device(), vk_.allocator());
    }
    if (sampler_) vkDestroySampler(vk_.device(), static_cast<VkSampler>(sampler_), nullptr);
}

const ItemIconCache::Sheet* ItemIconCache::sheet(int number) {
    auto it = sheets_.find(number);
    if (it != sheets_.end()) return it->second.get();
    std::unique_ptr<Sheet> s;
    std::string name = "uifiles/default/dragitem" + std::to_string(number) + ".dds";
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
            s->set = ui::add_imgui_texture(static_cast<VkSampler>(sampler_), s->texture.view,
                                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (!s->set) {
                log::warn("item icons: no descriptor left for {}", name);
                // Its upload may still be on the GPU.
                vk_.retire([texture = s->texture, device = vk_.device(), allocator = vk_.allocator()]() mutable {
                    texture.destroy(device, allocator);
                });
                s.reset();
            }
        } else {
            log::warn("item icons: {}: {}", name, error);
        }
    }
    return (sheets_[number] = std::move(s)).get();
}

bool ItemIconCache::draw(std::uint32_t icon, float size, const char* fallback_text) {
    auto cell = icon_cell(icon);
    const Sheet* s = cell ? sheet(cell->sheet) : nullptr;
    if (!s) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(size, size));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), IM_COL32(60, 60, 70, 255), 3.0f);
        if (fallback_text && *fallback_text) {
            char initial[2] = {fallback_text[0], 0};
            ImVec2 ts = ImGui::CalcTextSize(initial);
            dl->AddText(ImVec2(p.x + (size - ts.x) * 0.5f, p.y + (size - ts.y) * 0.5f), IM_COL32(230, 230, 230, 255), initial);
        }
        return false;
    }
    ImVec2 uv0(static_cast<float>(cell->x) / s->width, static_cast<float>(cell->y) / s->height);
    ImVec2 uv1(static_cast<float>(cell->x + 40) / s->width, static_cast<float>(cell->y + 40) / s->height);
    ImGui::Image(reinterpret_cast<ImTextureID>(s->set), ImVec2(size, size), uv0, uv1);
    return true;
}

bool ItemIconCache::draw_at(void* draw_list, std::uint32_t icon, float x, float y, float size) {
    auto cell = icon_cell(icon);
    const Sheet* s = cell ? sheet(cell->sheet) : nullptr;
    auto* dl = static_cast<ImDrawList*>(draw_list);
    if (!s) {
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + size, y + size), IM_COL32(60, 60, 70, 230), 3.0f);
        return false;
    }
    ImVec2 uv0(static_cast<float>(cell->x) / s->width, static_cast<float>(cell->y) / s->height);
    ImVec2 uv1(static_cast<float>(cell->x + 40) / s->width, static_cast<float>(cell->y + 40) / s->height);
    dl->AddImage(reinterpret_cast<ImTextureID>(s->set), ImVec2(x, y), ImVec2(x + size, y + size), uv0, uv1);
    return true;
}

// ---------------------------------------------------------------- item info

namespace {

const ImVec4 kDim{0.62f, 0.64f, 0.68f, 1.0f};
const ImVec4 kGold{1.0f, 0.85f, 0.35f, 1.0f};

const char* size_name(std::uint8_t size) {
    static constexpr const char* kSizes[] = {"TINY", "SMALL", "MEDIUM", "LARGE", "GIANT"};
    return size < 5 ? kSizes[size] : "GIANT";
}

const char* item_skill(std::uint8_t type) {
    switch (type) {
    case 0: return "1H Slashing";
    case 1: return "2H Slashing";
    case 2: return "1H Piercing";
    case 3: return "1H Blunt";
    case 4: return "2H Blunt";
    case 5: return "Archery";
    case 7: return "Throwing";
    case 8: return "Shield";
    case 10: return "Armor";
    case 35: return "2H Piercing";
    case 45: return "Hand to Hand";
    default: return nullptr;
    }
}

std::string classes_text(std::uint32_t mask) {
    if ((mask & 0xffff) == 0xffff) return "ALL";
    std::vector<std::uint32_t> ids;
    for (std::uint32_t bit = 0; bit < 16; ++bit)
        if (mask & (1u << bit)) ids.push_back(bit + 1);
    return ids.empty() ? "None" : game::class_list_name(ids, true);
}

std::string races_text(std::uint32_t mask) {
    if ((mask & 0xffff) == 0xffff || mask == 0) return "ALL";
    static constexpr const char* kRaces[] = {"HUM", "BAR", "ERU", "ELF", "HIE", "DEF", "HEF", "DWF",
                                              "TRL", "OGR", "HFL", "GNM", "IKS", "VAH", "FRG", "DRK"};
    std::string s;
    for (int bit = 0; bit < 16; ++bit)
        if (mask & (1u << bit)) s += (s.empty() ? "" : " ") + std::string(kRaces[bit]);
    return s;
}

std::string weight_text(std::int32_t tenths) { return std::format("{}.{}", tenths / 10, std::abs(tenths % 10)); }

} // namespace

std::vector<std::string> item_info_lines(const Item& it, const game::ItemFlagsFormatter& format_flags,
                                         const game::ItemDescriptionAppender& description_append) {
    std::vector<std::string> out;
    // Flags (CItemDisplayWnd_CreateItemFlagsString).
    std::string flags;
    auto flag = [&](bool on, const char* text) {
        if (!on) return;
        if (!flags.empty()) flags += ", ";
        flags += text;
    };
    flag(it.magic, "Magic");
    flag(it.is_lore(), "Lore");
    flag(it.artifact, "Artifact");
    flag(it.summoned, "Summoned");
    flag(it.no_trade(), "No Trade");
    flag(it.no_rent == 0, "Temporary");
    flag(it.attuneable, "Attunable");
    flag(it.quest_item, "Quest");
    flag(it.heirloom, "Heirloom");
    flag(it.placeable, "Placeable");
    flag(it.no_zone, "No Zone");
    flag(it.no_ground, "No Ground");
    if (format_flags) if (auto changed = format_flags(it, flags)) flags = std::move(*changed);
    if (!flags.empty()) out.push_back(flags);
    if (it.item_class == 0) {
        out.push_back("Class: " + classes_text(it.classes));
        out.push_back("Race: " + races_text(it.races));
    }
    out.push_back(std::format("Size: {}    Weight: {}", size_name(it.size), weight_text(it.weight)));
    if (it.is_bag()) {
        out.push_back(std::format("Weight Red: {}%    Capacity: {}    Size Cap: {}", it.weight_reduction, it.bag_slots,
                                  size_name(it.bag_size)));
    }
    if (it.rec_level) out.push_back(std::format("Rec Level: {}", it.rec_level));
    if (it.req_level) out.push_back(std::format("Req Level: {}", it.req_level));
    if (const char* skill = it.item_class == 0 ? item_skill(it.item_type) : nullptr) out.push_back(std::string("Skill: ") + skill);
    auto num = [&](const char* label, std::int64_t v, std::int64_t heroic = 0) {
        if (!v && !heroic) return;
        if (heroic) out.push_back(std::format("{} {:+} +{}", label, v, heroic));
        else out.push_back(std::format("{} {}", label, v));
    };
    num("AC:", it.ac);
    num("HP:", it.hp);
    num("Mana:", it.mana);
    num("End:", it.endurance);
    if (it.haste) out.push_back(std::format("Haste: +{}%", it.haste));
    if (it.damage) out.push_back(std::format("Base Dmg: {}    Delay: {}", it.damage, it.delay));
    num("Dmg Bon:", it.elem_dmg_amt);
    num("Range:", it.range);
    num("Strength:", it.astr, it.heroic_str);
    num("Stamina:", it.asta, it.heroic_sta);
    num("Intelligence:", it.aint, it.heroic_int);
    num("Wisdom:", it.awis, it.heroic_wis);
    num("Agility:", it.aagi, it.heroic_agi);
    num("Dexterity:", it.adex, it.heroic_dex);
    num("Charisma:", it.acha, it.heroic_cha);
    num("Magic:", it.mr, it.heroic_mr);
    num("Fire:", it.fr, it.heroic_fr);
    num("Cold:", it.cr, it.heroic_cr);
    num("Disease:", it.dr, it.heroic_dr);
    num("Poison:", it.pr, it.heroic_pr);
    num("Corrupt:", it.sv_corruption, it.heroic_sv_corrup);
    num("Attack:", it.attack);
    num("HP Regen:", it.regen);
    num("Mana Regen:", it.mana_regen);
    num("End Regen:", it.end_regen);
    num("Spell Shield:", it.spell_shield);
    num("Combat Eff:", it.combat_effects);
    num("Shielding:", it.shielding);
    num("Dmg Shield:", it.damage_shield);
    num("DoT Shield:", it.dot_shield);
    num("Dmg Shld Mit:", it.ds_mitigation);
    num("Avoidance:", it.avoidance);
    num("Accuracy:", it.accuracy);
    num("Stun Resist:", it.stun_resist);
    num("Strike Thr:", it.strike_through);
    num("Heal Amount", it.heal_amt);
    num("Spell Dmg:", it.spell_dmg);
    num("Clairvoyance:", it.clairvoyance);
    // Spell blocks (CreateEffectsString).
    static constexpr const char* kKinds[] = {"Spell Effect", "Combat Effect", "Worn Effect", "Focus Effect",
                                             "Scroll Effect", "Bard Effect"};
    for (std::size_t k = 0; k < net::kEffectCount; ++k) {
        const auto& e = it.effects[k];
        if (!e.present()) continue;
        std::string name = e.name.empty() ? std::format("spell {}", e.effect) : e.name;
        out.push_back(std::format("{}:  {}", kKinds[k], name));
    }
    if (it.effects[net::kEffectClick].present()) {
        if (it.charges == 0xffffffff) out.push_back("Charges: Unlimited");
        else if (it.charges) out.push_back(std::format("Charges: {}", it.charges));
    }
    if (it.stackable()) out.push_back(std::format("Stack: {} / {}", it.stack_count, it.max_stack));
    if (it.value) {
        std::uint32_t v = it.value;
        out.push_back("Value: " + game::coin_text(v / 1000, v / 100 % 10, v / 10 % 10, v % 10));
    }
    if (!it.lore.empty() && it.lore != it.name && it.lore[0] != '*') out.push_back("Lore: " + it.lore);
    out.push_back(std::format("Item ID: {}", it.id));
    if (description_append)
        if (auto text = description_append(it); text && !text->empty()) {
            // The fallback draws plain lines with TextUnformatted, so markup
            // stays literal. CRLF is one break, as in the shared STML display.
            std::size_t start = 0;
            while (start < text->size()) {
                const auto end = text->find_first_of("\r\n", start);
                out.push_back(text->substr(start, end == std::string::npos ? end : end - start));
                if (end == std::string::npos) break;
                start = end + 1;
                if ((*text)[end] == '\r' && start < text->size() && (*text)[start] == '\n') ++start;
            }
        }

    return out;
}

void show_item_info(ItemWindowsState& state, Item item) {
    // One window per item id: a second request brings the same window back.
    for (std::size_t i = 0; i < state.info.size(); ++i) {
        if (state.info[i].id == item.id) {
            state.info[i] = std::move(item);
            return;
        }
    }
    state.info.push_back(std::move(item));
    state.info_ids.push_back(++state.info_serial);
    if (state.info.size() > 6) { // the Lua item displays: ItemDisplayWindow .. #6
        state.info.erase(state.info.begin());
        state.info_ids.erase(state.info_ids.begin());
    }
}

void refresh_item_info(ItemWindowsState& state, const Item& item) {
    if (item.instance_id == 0 && item.parcel_item_id == 0) return;
    for (auto& shown : state.info)
        // Evolving database ids and ordinary item serials are separate namespaces in RoF2 serialization.
        if ((shown.is_evolving != 0) == (item.is_evolving != 0) && shown.instance_id == item.instance_id &&
            shown.parcel_item_id == item.parcel_item_id)
            shown = item;
}

// ---------------------------------------------------------------- windows

namespace {

constexpr float kSlotEm = 2.6f;

std::string item_tooltip(const Item& it) {
    std::string s = it.name;
    if (it.stackable() && it.stack_count > 1) s += std::format(" ({})", it.stack_count);
    return s;
}

// One inventory slot: an invisible button with the icon drawn over it.
// Returns true on a left click; *right_clicked reports a right click.
bool slot_button(const char* id, const Item* item, const char* empty_label, ItemIconCache* icons, bool* right_clicked) {
    float size = ImGui::GetFontSize() * kSlotEm;
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("slot", ImVec2(size, size), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();
    bool left = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    bool right = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), IM_COL32(25, 25, 30, 220), 3.0f);
    if (item) {
        bool drawn = icons && icons->draw_at(dl, item->icon, p.x + 2, p.y + 2, size - 4);
        if (!drawn) dl->AddText(ImVec2(p.x + 3, p.y + 3), IM_COL32(230, 230, 230, 255), item->name.substr(0, 5).c_str());
        if (item->stackable() && item->stack_count >= 1) {
            std::string n = std::to_string(item->stack_count);
            ImVec2 ts = ImGui::CalcTextSize(n.c_str());
            dl->AddText(ImVec2(p.x + size - ts.x - 3, p.y + size - ts.y - 1), IM_COL32(255, 255, 255, 255), n.c_str());
        }
    } else if (empty_label) {
        ImFont* font = ImGui::GetFont();
        float fs = ImGui::GetFontSize() * 0.75f;
        ImVec2 ts = font->CalcTextSizeA(fs, size, 0.0f, empty_label);
        dl->AddText(font, fs, ImVec2(p.x + (size - ts.x) * 0.5f, p.y + (size - ts.y) * 0.5f), IM_COL32(110, 110, 120, 255),
                    empty_label);
    }
    dl->AddRect(p, ImVec2(p.x + size, p.y + size), hovered ? IM_COL32(220, 200, 120, 255) : IM_COL32(90, 90, 100, 255), 3.0f);
    if (hovered && item) ImGui::SetTooltip("%s", item_tooltip(*item).c_str());
    ImGui::PopID();
    if (right_clicked) *right_clicked = right;
    return left;
}

const char* short_slot(int slot) {
    static constexpr const char* kShort[] = {"Charm", "Ear", "Head", "Face", "Ear", "Neck", "Shldr", "Arms", "Back",
                                             "Wrist", "Wrist", "Range", "Hands", "Prim", "Sec", "Ring", "Ring",
                                             "Chest", "Legs", "Feet", "Waist", "Power", "Ammo"};
    return slot >= 0 && slot < 23 ? kShort[slot] : "";
}

} // namespace

ItemWindowsActions item_windows(ItemWindowsState& state, const ItemWindowsInput& in, ItemIconCache* icons) {
    ItemWindowsActions act;
    const game::Inventory* inv = in.inventory;
    float em = ImGui::GetFontSize();
    float slot = em * kSlotEm;
    float gap = ImGui::GetStyle().ItemSpacing.x * 0.5f;

    auto inspect_or_open = [&](const ItemSlot& s, const Item* item, bool right) {
        if (!right || !item) return;
        if (item->is_bag() && s.type == net::kInvPossessions && s.sub < 0 && s.slot >= net::kSlotGeneral1 &&
            s.slot <= net::kSlotGeneral10) {
            // CInvSlot::HandleRButtonUp: right click opens or closes a bag.
            if (!state.open_bags.erase(s.slot)) state.open_bags.insert(s.slot);
        } else {
            show_item_info(state, *item);
        }
    };

    // ---- inventory
    if (state.show_inventory && inv && !lua::replaces("InventoryWindow")) { // off while the Lua window runs
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - em * 26.0f, vp->WorkPos.y + em * 14.0f),
                                ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.85f);
        if (ImGui::Begin("Inventory##hailstone_inv", &state.show_inventory,
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
            // Equipment: the stock window's 23 slots, six per row.
            ImGui::TextColored(kDim, "Equipment");
            for (int s = 0; s < net::kEquipmentSlots; ++s) {
                if (s % 6) ImGui::SameLine(0.0f, gap);
                ItemSlot is = ItemSlot::possessions(static_cast<std::int16_t>(s));
                const Item* item = inv->at(is);
                bool right = false;
                if (slot_button(std::format("hs{}", s).c_str(), item, short_slot(s), icons, &right)) act.click = is;
                inspect_or_open(is, item, right);
            }
            ImGui::TextColored(kDim, "General (right click a bag to open it)");
            for (int s = net::kSlotGeneral1; s <= net::kSlotGeneral10; ++s) {
                if ((s - net::kSlotGeneral1) % 5) ImGui::SameLine(0.0f, gap);
                ItemSlot is = ItemSlot::possessions(static_cast<std::int16_t>(s));
                const Item* item = inv->at(is);
                bool right = false;
                if (slot_button(std::format("gen{}", s).c_str(), item, nullptr, icons, &right)) act.click = is;
                inspect_or_open(is, item, right);
            }
            ImGui::Text("%u pp  %u gp  %u sp  %u cp", in.coins.platinum, in.coins.gold, in.coins.silver, in.coins.copper);
            const Item* held = inv->cursor();
            ImGui::BeginDisabled(!held);
            if (ImGui::Button("Auto equip / inventory")) act.auto_inventory = true;
            ImGui::SameLine();
            if (ImGui::Button("Destroy held item")) ImGui::OpenPopup("destroy?");
            ImGui::EndDisabled();
            if (ImGui::BeginPopup("destroy?")) {
                ImGui::Text("Destroy %s?", held ? held->name.c_str() : "");
                if (ImGui::Button("Destroy")) {
                    act.destroy = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            if (held) ImGui::TextColored(kGold, "Holding: %s", item_tooltip(*held).c_str());
        }
        ImGui::End();
    }

    // ---- open bags (client-only: the original sends nothing for inventory bags)
    if (inv && !lua::replaces("Bag1")) {
        for (auto it = state.open_bags.begin(); it != state.open_bags.end();) {
            std::int16_t s = *it;
            const Item* bag = inv->at(ItemSlot::possessions(s));
            if (!bag || !bag->is_bag()) {
                it = state.open_bags.erase(it);
                continue;
            }
            bool open = true;
            ImGui::SetNextWindowBgAlpha(0.85f);
            // Left of the inventory window, one below the other by general slot.
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - em * 38.0f,
                                           vp->WorkPos.y + em * (14.0f + 7.0f * static_cast<float>(s - net::kSlotGeneral1) * 0.5f)),
                                    ImGuiCond_Appearing);
            if (ImGui::Begin(std::format("{}##hailstone_bag{}", bag->name, s).c_str(), &open,
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
                for (int i = 0; i < bag->bag_slots; ++i) {
                    if (i % 4) ImGui::SameLine(0.0f, gap);
                    ItemSlot is = ItemSlot::possessions(s, static_cast<std::int16_t>(i));
                    const Item* item = inv->at(is);
                    bool right = false;
                    if (slot_button(std::format("b{}", i).c_str(), item, nullptr, icons, &right)) act.click = is;
                    inspect_or_open(is, item, right);
                }
            }
            ImGui::End();
            if (!open) it = state.open_bags.erase(it);
            else ++it;
        }
    }

    // ---- loot window (CLootWnd)
    if (in.corpse_id && in.loot && !lua::replaces("LootWnd")) {
        ImGui::SetNextWindowBgAlpha(0.85f);
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f - em * 10.0f, vp->WorkPos.y + em * 8.0f),
                                ImGuiCond_FirstUseEver);
        bool open = true;
        if (ImGui::Begin(std::format("Loot: {}###hailstone_loot", in.corpse_name).c_str(), &open,
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
            if (in.loot->empty()) ImGui::TextColored(kDim, "The corpse is empty.");
            int n = 0;
            for (const auto& [cs, item] : *in.loot) {
                if (n++ % 4) ImGui::SameLine(0.0f, gap);
                bool right = false;
                if (slot_button(std::format("l{}", cs).c_str(), &item, nullptr, icons, &right)) act.loot = {cs, false};
                if (right) act.loot = {cs, true}; // right click loots straight into the inventory
            }
            ImGui::TextColored(kDim, "Left click: to cursor. Right click: to inventory.");
            ImGui::BeginDisabled(in.loot->empty());
            if (ImGui::Button("Loot all")) act.loot_all = true;
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Done")) act.close_loot = true;
        }
        ImGui::End();
        if (!open) act.close_loot = true;
    }

    // ---- item information windows (CItemDisplayWnd)
    for (std::size_t i = 0; i < state.info.size() && !lua::replaces("ItemDisplayWindow");) {
        const Item& it = state.info[i];
        bool open = true;
        ImGui::SetNextWindowBgAlpha(0.9f);
        ImVec2 at = ImGui::IsMousePosValid() ? ImVec2(ImGui::GetMousePos().x + 16, ImGui::GetMousePos().y)
                                             : ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(at, ImGuiCond_Appearing);
        if (ImGui::Begin(std::format("{}###hailstone_info{}", it.name, state.info_ids[i]).c_str(), &open,
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
            if (icons) icons->draw(it.icon, em * kSlotEm, it.name.c_str());
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextColored(kGold, "%s", it.name.c_str());
            for (const auto& line : item_info_lines(it, in.item_flags, in.item_description_append)) ImGui::TextUnformatted(line.c_str());
            ImGui::EndGroup();
        }
        ImGui::End();
        if (!open) {
            state.info.erase(state.info.begin() + static_cast<std::ptrdiff_t>(i));
            state.info_ids.erase(state.info_ids.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }

    // ---- the item on the cursor follows the mouse
    if (inv) {
        if (const Item* held = inv->cursor(); held && ImGui::IsMousePosValid() && !lua::replaces("CursorAttachment")) {
            ImVec2 m = ImGui::GetMousePos();
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            if (!icons || !icons->draw_at(fg, held->icon, m.x + 4, m.y + 4, slot * 0.8f))
                fg->AddText(ImVec2(m.x + 8, m.y + 8), IM_COL32(255, 230, 150, 255), held->name.c_str());
        }
    }
    return act;
}

} // namespace hailstone::ui
