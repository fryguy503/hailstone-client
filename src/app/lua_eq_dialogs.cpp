// Small dialog windows: QuantityWnd (CQuantityWnd).

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <format>
#include <future>
#include <optional>
#include <string>
#include <utility>

#include "app/lua_eq_windows.h"
#include <imgui.h>

#include "app/actions_ui.h"
#include "game/coords.h"
#include "game/chat_windows.h"
#include "game/eq_names.h"
#include "game/keymap.h"
#include "game/mechanics/character.h"
#include "game/quantity_dialog.h"
#include "game/spell_display.h"
#include "game/spells.h"
#include "game/target_indicator.h"
#include "net/game_connection.h"
#include "net/spell_packets.h"
#include "platform/open_folder.h"

namespace hailstone::app {

namespace {

// CQuantityWnd @0x7255a0: a slider over min..max (SetNumTicks(max + 1 - min), the value min-relative) and an edit
// box that follow each other (sub_00725940 writes the slider's value into the box; sub_00725a20 reads the box back
// into the slider), Accept (or Enter in the box) hands the amount to the owner (sub_00725ab0: slider + min). Two
// owners open it here: the connection, for a stack picked up without shift or ctrl (GameConnection::click_slot,
// quantity_request(), min 0), and a window through open_quantity() (EqHost::quantity: the merchant's Buy / Sell,
// the trader's money buttons, ...). CQuantityWnd::Open moves it to its caller's point (CInvSlot: the slot's centre)
// kept on the screen, or centres it; a closed window cancels.
class QuantityWindow : public StockWindow {
public:
    QuantityWindow() : StockWindow("QuantityWnd", false) {}

    void frame(EqHost& host) override { sync(host); }

    bool shown(EqHost& host) override {
        sync(host);
        return active_;
    }
    void set_shown(EqHost& host, bool show) override {
        if (show) return;
        if (host.quantity) host.quantity.reset();
        else if (host.conn) host.conn->cancel_quantity();
        sync(host);
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        sync(host);
        if (!active_) return;
        if (id == "QTYW_Slider") {
            out.slider_count = max_ - min_ + 1;
            out.slider_value = value_;
        } else if (id == "QTYW_SliderInput") {
            out.text = text_;
        }
    }

    bool set_slider(EqHost& host, std::string_view id, int value) override {
        sync(host);
        if (!active_ || id != "QTYW_Slider") return false;
        value_ = std::clamp(value, 0, max_ - min_);
        text_ = std::to_string(value_ + min_); // sub_00725940
        return true;
    }

    bool set_text(EqHost& host, std::string_view id, const std::string& text) override {
        sync(host);
        if (!active_ || id != "QTYW_SliderInput") return false;
        text_ = text;
        return true;
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        sync(host);
        if (!active_ || button != "left") return false;
        if (id == "QTYW_Accept_Button" || id == "QTYW_SliderInput") {
            // sub_00725a20: the box's number (atoi) into the slider, clamped by it; then slider + min to the owner.
            int v = 0;
            std::from_chars(text_.data(), text_.data() + text_.size(), v);
            value_ = std::clamp(v - min_, 0, max_ - min_);
            std::int32_t q = game::quantity_dialog_accept(min_, value_);
            if (host.quantity) {
                auto done = std::move(host.quantity->done);
                host.quantity.reset();
                if (done) done(q);
            } else if (host.conn) {
                host.conn->finish_quantity(static_cast<std::uint32_t>(q));
            }
            sync(host);
            return true;
        }
        return false;
    }

    std::optional<std::array<int, 2>> take_move(EqHost&) override {
        auto m = move_;
        move_.reset();
        return m;
    }

private:
    // The dialog's owner this frame; a new one starts the slider at its start and moves the window.
    void sync(EqHost& host) {
        if (host.quantity && host.quantity->owner_valid && !host.quantity->owner_valid()) host.quantity.reset();
        int key = 0;
        std::int32_t min = 0, max = 0, start = 0;
        std::optional<std::pair<int, int>> pos;
        if (host.quantity) {
            key = host.quantity->serial;
            min = host.quantity->min, max = host.quantity->max, start = host.quantity->slider;
            pos = host.quantity->position;
        } else if (host.conn && host.conn->quantity_request()) {
            const auto& q = *host.conn->quantity_request();
            key = -1;
            max = static_cast<std::int32_t>(q.max), start = static_cast<std::int32_t>(q.start);
            if (!conn_open_ || !for_ || !for_->same(q.slot) || conn_max_ != q.max) {
                // CInvSlot::HandleLButtonUp opens it at the slot's centre; CLootWnd::RequestLootSlot at (-1, -1),
                // centred.
                const auto& r = host.click_rect;
                if (r[0] >= 0 && !q.centred) pos = std::pair{static_cast<int>((r[0] + r[2]) / 2), static_cast<int>((r[1] + r[3]) / 2)};
                for_ = q.slot;
                conn_max_ = q.max;
                conn_open_ = false;
            }
        }
        active_ = key != 0;
        if (!active_) {
            key_ = 0;
            conn_open_ = false;
            for_.reset();
            return;
        }
        if (key == key_ && (key != -1 || conn_open_)) return;
        key_ = key;
        if (key == -1) conn_open_ = true;
        min_ = min, max_ = std::max(min, max);
        value_ = std::clamp(start, 0, max_ - min_);
        text_ = std::to_string(value_ + min_);
        move_.reset();
        if (pos) {
            // CQuantityWnd::Open: kept on the screen by the dialog's own size.
            ImVec2 ds = ImGui::GetIO().DisplaySize;
            auto r = host.textures ? host.textures->screen_rect("QuantityWnd") : std::nullopt;
            int w = r ? r->w : 200, h = r ? r->h : 100;
            int x = pos->first, y = pos->second;
            if (x + w > static_cast<int>(ds.x)) x = static_cast<int>(ds.x) - w;
            if (y + h > static_cast<int>(ds.y)) y = static_cast<int>(ds.y) - h;
            move_ = std::array<int, 2>{x, y};
        } else {
            ImVec2 ds = ImGui::GetIO().DisplaySize;
            auto r = host.textures ? host.textures->screen_rect("QuantityWnd") : std::nullopt;
            int w = r ? r->w : 200, h = r ? r->h : 100;
            move_ = std::array<int, 2>{(static_cast<int>(ds.x) - w) / 2, (static_cast<int>(ds.y) - h) / 2};
        }
    }

    bool active_ = false, conn_open_ = false;
    int key_ = 0;
    std::optional<net::ItemSlot> for_;
    std::uint32_t conn_max_ = 0;
    std::int32_t min_ = 0, max_ = 0;
    int value_ = 0;
    std::string text_;
    std::optional<std::array<int, 2>> move_;
};

// The Colors page's rows (the table at 0xb39450: user colour - 256, eqstr name, hidden); hidden rows are left out
// (sub_00700bb0).
struct OptionRow {
    int data;
    std::uint32_t eqstr;
    int hidden;
};
constexpr OptionRow kColorRows[] = {
    {0, 6200, 0}, {1, 6201, 0}, {2, 6202, 0}, {3, 6203, 0}, {4, 6204, 0}, {5, 6205, 0}, {6, 6206, 0}, {7, 6207, 0},
    {8, 6208, 0}, {9, 6209, 0}, {10, 6210, 0}, {11, 6211, 0}, {12, 6212, 0}, {13, 6213, 0}, {14, 6214, 0},
    {15, 6215, 0}, {16, 6216, 0}, {17, 6217, 0}, {18, 6218, 0}, {19, 6219, 0}, {20, 6220, 0}, {21, 6221, 0},
    {22, 6222, 0}, {23, 6223, 0}, {24, 6224, 0}, {25, 6225, 0}, {26, 6226, 0}, {27, 6227, 0}, {28, 6228, 0},
    {29, 6229, 0}, {30, 6230, 0}, {31, 6231, 0}, {32, 6232, 0}, {33, 6233, 0}, {34, 6234, 0}, {35, 6235, 0},
    {36, 6236, 0}, {37, 6237, 0}, {38, 6238, 0}, {39, 6239, 0}, {40, 6240, 0}, {41, 6241, 0}, {42, 6242, 0},
    {43, 6243, 0}, {44, 6244, 0}, {45, 6245, 0}, {46, 6246, 0}, {47, 6247, 0}, {48, 6248, 0}, {49, 6249, 0},
    {50, 6250, 0}, {51, 6251, 0}, {52, 6252, 0}, {53, 6253, 0}, {54, 6254, 0}, {55, 6255, 0}, {56, 6256, 0},
    {57, 6257, 0}, {58, 6258, 0}, {59, 6259, 0}, {60, 6260, 0}, {61, 6261, 0}, {62, 6262, 0}, {63, 6263, 0},
    {64, 6264, 0}, {65, 6265, 0}, {66, 6266, 0}, {67, 6267, 0}, {68, 6268, 0}, {69, 6269, 8}, {70, 6270, 0},
    {71, 6271, 0}, {72, 6272, 0}, {73, 6273, 0}, {74, 6274, 0}, {75, 6275, 0}, {76, 6276, 0}, {77, 6277, 0},
    {78, 6278, 0}, {79, 6279, 0}, {80, 6280, 0}, {81, 6281, 0}, {82, 6151, 0}, {83, 6285, 0}, {84, 6286, 0},
    {86, 4586, 0}, {87, 641, 0}, {88, 642, 0}, {89, 645, 0}, {90, 6360, 0}, {91, 7506, 0}, {92, 5208, 0},
    {93, 5209, 0}, {94, 6450, 0}, {95, 7680, 0}, {96, 7725, 0}, {97, 7726, 0}, {98, 7727, 0},
};
// COptionsWnd. The Options window itself is this client's own
// (ui/<pack>/options.lua: its settings are client options, eq.Option / eq.SetOption); it takes these lists from here:
//  - Keys (KeyboardPageWndNotification @0x70aa40, RefreshCurrentKeyboardAssignmentList @0x70a560): the filter
//    combo's choices are the XML's (Movement .. Voice Chat, All; not Item Placement, as real estate is left out:
//    game::key_command_dropped), choice n lists the commands of category bit 1 << n (All: 0x7ffff), in COptionsWnd::InitKeyboardAssignments' order; the columns are the command's label,
//    its key and its alternate key (KeyCombo::GetTextDescription). A click on a key column shows eqstr 6598 there and
//    takes the next key (sub_0070abc0: modifiers alone are ignored, Escape unbinds); Default Keys asks eqstr 6602
//    (ConfirmationDialog 100, Yes / No, 20 s) and resets the bindings.
//  - Chat filters (COptionsWnd::InitChatPage): every filter with its setting, and the selected filter's choices.
//  - Colors (ColorPageWndNotification, SyncColorPage): the user colours of the table at 0xb39450 with "Sample Text"
//    (eqstr 6134) in each colour (black shown white); Change Selected Color opens the colour picker on the row's
//    colour and SaveColors stores the answer; Reset Default Colors asks eqstr 6602 (20 s) and restores the 99
//    defaults (RestoreDefaultColors). Hailstone: the 54 extended colours (game/chat_windows.h) follow the
//    original's rows, by Hailstone's names, and the reset restores them too.
class OptionsWindow : public StockWindow {
public:
    OptionsWindow() : StockWindow("OptionsWindow", false) {}

    void child(EqHost&, std::string_view id, Child& out) override {
        if (id == "UserColorChange") out.enabled = color_row_ >= 0; // SyncColorPage
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (button != "left") return false;
        if (id == "UserColorChange") {
            // ColorPageWndNotification: CColorPickerWnd::Open(this, the row's sample colour); its 0x25 is SaveColors.
            if (color_row_ < 0 || color_row_ >= static_cast<int>(color_data_.size())) return true;
            int index = color_data_[static_cast<std::size_t>(color_row_)];
            host.pick_color(sample_rgb(index), [&host, index](std::uint32_t c) { set_user_color(host, index, c & 0xffffff); });
            return true;
        }
        if (id.starts_with("TargetRingColor")) {
            // Hailstone's Target Ring rows (ClientApp ring options): TargetRingColor<n> opens the colour picker on con
            // section n's colour and keeps the answer; TargetRingColorDefault puts every section back to the ini's.
            if (!host.find_option || !host.set_option) return true;
            std::string_view rest = id.substr(std::string_view("TargetRingColor").size());
            if (rest == "Default") {
                for (int i = 0; i < game::kTargetRingConSections; ++i)
                    host.set_option(std::format("target_ring_color_{}", i), -1);
                return true;
            }
            if (rest.size() != 1 || rest[0] < '0' || rest[0] >= '0' + game::kTargetRingConSections) return true;
            std::string name = std::format("target_ring_color_{}", rest);
            std::optional<int> now = host.find_option(name);
            if (!now) return true;
            host.pick_color(static_cast<std::uint32_t>(*now), [&host, name](std::uint32_t argb) {
                host.set_option(name, static_cast<int>(argb & 0xffffffu));
            });
            return true;
        }
        if (id == "UserColorDefault") {
            // ConfirmationDialog(0x65, eqstr 6602, Yes / No, 20 s); Yes: RestoreDefaultColors (all 99 to the defaults;
            // Hailstone: the extended ones too).
            if (!host.conn) return true;
            EqHost::Confirm c;
            c.text = host.conn->strings().format(6602, {});
            c.timeout_ms = 20000;
            c.done = [&host](bool yes) {
                if (!yes) return;
                for (int i = 0; i < game::kChatUserColorCount; ++i) set_user_color(host, i, default_user_color_rgb(i));
            };
            host.confirm = std::move(c);
            return true;
        }
        if (id != "OKP_DefaultKeysButton") return false;
        if (!host.conn || !host.actions) return true;
        EqHost::Confirm c;
        c.text = host.conn->strings().format(6602, {});
        c.timeout_ms = 20000;
        c.done = [&host](bool yes) {
            if (yes && host.actions) host.actions->reset_keys();
        };
        host.confirm = std::move(c);
        return true;
    }

    const ComboData* combo(EqHost& host, std::string_view id) override {
        if (id == "FilterComboBox") {
            // The chat page: the selected filter's settings (by its mode: show, hide, all / your own / your group's).
            chat_choice_ = {};
            if (!host.conn || chat_filter_ < 0) return &chat_choice_;
            for (int s = 0; s < game::ChatFilters::options(chat_filter_); ++s)
                chat_choice_.choices.push_back(setting_text(host, chat_filter_, s));
            chat_choice_.selected = host.conn->chat_filters().settings[static_cast<std::size_t>(chat_filter_)];
            return &chat_choice_;
        }
        if (id != "OKP_KeyboardFilterCombobox") return nullptr;
        static const char* const kChoices[] = {"Movement", "Commands", "Spell casting", "Target", "Camera", "Chat", "UI",
                                               "Hotbar 1", "Hotbar 2", "Hotbar 3", "Hotbar 4", "Hotbar 5", "Hotbar 6",
                                               "Hotbar 7", "Hotbar 8", "Hotbar 9", "Hotbar 10", "Voice Chat",
                                               "All"}; // (not the original's "Item Placement": real estate is left out)
        filter_.choices.assign(std::begin(kChoices), std::end(kChoices));
        filter_.selected = key_filter_;
        return &filter_;
    }
    bool combo_select(EqHost& host, std::string_view id, int index) override {
        if (id == "FilterComboBox") {
            if (host.conn && chat_filter_ >= 0) host.conn->change_chat_filter(chat_filter_, index);
            return true;
        }
        if (id != "OKP_KeyboardFilterCombobox") return false;
        key_filter_ = index;
        return true;
    }

    const ListData* list(EqHost& host, std::string_view id) override {
        if (id == "ColorChooseList") {
            // Filled only with a local player (sub_00700bb0); column 1 as SyncColorPage sets it.
            if (!host.conn || !host.in_game) return nullptr;
            color_list_ = {};
            color_list_.columns = 2;
            color_list_.labels = {"Text description", "Color Sample"};
            color_list_.widths = {150, 150};
            color_data_.clear();
            for (const OptionRow& r : kColorRows) {
                if (r.hidden != 0) continue;
                ListRow row;
                row.texts = {host.conn->strings().format(r.eqstr, {}), host.conn->strings().format(6134, {})};
                row.cell_colors = {std::nullopt, argb_to_rgb(0xff000000u | sample_rgb(r.data))};
                color_data_.push_back(r.data);
                color_list_.rows.push_back(std::move(row));
            }
            for (int index = game::kChatUserColorCountRoF2; index < game::kChatUserColorCount; ++index) {
                ListRow row;
                row.texts = {std::string(extended_chat_color_name(index)), host.conn->strings().format(6134, {})};
                row.cell_colors = {std::nullopt, argb_to_rgb(0xff000000u | sample_rgb(index))};
                color_data_.push_back(index);
                color_list_.rows.push_back(std::move(row));
            }
            color_list_.selected = color_row_;
            return &color_list_;
        }
        if (id == "FilterList") {
            // The chat page: the filter table's rows (sub_00700bb0, only with a local player) by their eqstr names,
            // each with its setting (SyncChatPage).
            chat_list_ = {};
            chat_list_.columns = 2;
            chat_list_.labels = {"Filter", "Setting"};
            chat_list_.widths = {180, 90};
            chat_list_.selected = -1;
            if (!host.conn || !host.in_game) return &chat_list_;
            for (const auto& [filter, name] : kChatFilterRows) {
                if (filter == chat_filter_) chat_list_.selected = static_cast<int>(chat_list_.rows.size());
                chat_list_.rows.push_back(ListRow{{host.conn->strings().format(name, {}),
                                                   setting_text(host, filter, host.conn->chat_filters().settings[static_cast<std::size_t>(filter)])}});
            }
            return &chat_list_;
        }
        if (id != "OKP_KeyboardAssignmentList" || !host.actions || !host.conn) return nullptr;
        std::uint32_t mask = key_filter_ >= 0 && key_filter_ < 18 ? 1u << key_filter_ : 0x7ffffu;
        keys_ = {};
        keys_.columns = 3;
        keys_.labels = {"Command", "Keypress", "Alternate"};
        keys_.widths = {180, 75, 75};
        commands_.clear();
        auto capture = host.actions->key_capture();
        const auto& km = host.actions->keymap();
        // Hailstone: a key bound to more than one command shows red in every row that has it (a press runs them all).
        const std::vector<std::int32_t> shared = km.shared_combos();
        auto shared_color = [&](const game::KeyCombo& k) -> std::optional<Rgb> {
            if (!k.bound() || !std::binary_search(shared.begin(), shared.end(), k.to_int())) return std::nullopt;
            return kSharedKey;
        };
        for (int cmd : game::listed_commands()) {
            if ((game::command_category(cmd) & mask) == 0) continue;
            std::uint32_t label = game::command_label_eqstr(cmd);
            ListRow r;
            r.texts = {label ? host.conn->strings().format(label, {}) : std::string(game::command_name(cmd)),
                       game::key_text(km.binding(cmd, 0)), game::key_text(km.binding(cmd, 1))};
            r.cell_colors = {std::nullopt, shared_color(km.binding(cmd, 0)), shared_color(km.binding(cmd, 1))};
            if (capture.first == cmd) {
                auto c = static_cast<std::size_t>(1 + capture.second);
                r.texts[c] = host.conn->strings().format(6598, {});
                r.cell_colors[c] = std::nullopt;
            }
            if (cmd == selected_cmd_) keys_.selected = static_cast<int>(keys_.rows.size());
            commands_.push_back(cmd);
            keys_.rows.push_back(std::move(r));
        }
        return &keys_;
    }
    bool list_click(EqHost& host, std::string_view id, int row, bool right, int column) override {
        if (id == "ColorChooseList") {
            color_row_ = row;
            return true;
        }
        if (id == "FilterList") {
            // the row's item data: its filter
            chat_filter_ = row >= 0 && row < static_cast<int>(std::size(kChatFilterRows))
                               ? kChatFilterRows[static_cast<std::size_t>(row)].first
                               : -1;
            return true;
        }
        if (id != "OKP_KeyboardAssignmentList" || !host.actions) return false;
        if (row < 0 || row >= static_cast<int>(commands_.size())) return true;
        selected_cmd_ = commands_[static_cast<std::size_t>(row)];
        host.actions->cancel_key_capture(); // KeyboardPageCancelKeypressAssignment
        // A command above 0x1cf cannot be bound here (sub_0070abc0 cancels).
        if (!right && column > 0 && selected_cmd_ <= 0x1cf) host.actions->capture_key(selected_cmd_, column - 1, true);
        return true;
    }

    std::optional<int> tab_page(EqHost&, std::string_view id) override {
        if (id == "OPTW_OptionsSubwindows") return page_;
        return std::nullopt;
    }
    void set_tab_page(EqHost&, std::string_view id, int page) override {
        if (id == "OPTW_OptionsSubwindows") page_ = page;
    }

private:
    static constexpr Rgb kSharedKey{1.0f, 0.3f, 0.3f};
    // The chat page's filter list (sub_00700bb0 over the table at 0xb392c0, 0xb392bc rows of {filter, eqstr, flag};
    // all with flag 0; Swarm Pet Deaths, filter 28, has none) in the window's order.
    static constexpr std::pair<int, std::uint32_t> kChatFilterRows[] = {
        {6, 6300},  {0, 6306},  {1, 6307},  {2, 6308},  {3, 6309},  {4, 6310},  {5, 6311},  {7, 6301},  {8, 6317},
        {10, 6303}, {9, 6302},  {12, 6305}, {18, 6318}, {23, 6323}, {21, 6321}, {11, 6304}, {13, 6312}, {16, 6315},
        {14, 6313}, {15, 6314}, {19, 6319}, {20, 6320}, {22, 6322}, {17, 6316}, {24, 6152}, {25, 6288}, {26, 6289},
        {27, 6164}, {29, 4588}, {30, 6362}, {31, 6378}, {32, 5207}, {33, 6449}};
    // ChatFilters_GetOptionValue @0x8cf300: a setting's text, the eqstr of its mode (the table at 0xa1e370: Show
    // 6136, Hide 6135, Show 1347, Show Mine Only 1350, Show Group Only 1349).
    static std::string setting_text(EqHost& host, int filter, std::int32_t setting) {
        static constexpr std::uint32_t kValues[] = {6136, 6135, 1347, 1350, 1349};
        std::int32_t mode = game::ChatFilters::mode_of(filter, setting);
        if (!host.conn || mode < 0 || mode > 4) return {};
        return host.conn->strings().format(kValues[mode], {});
    }
    // SyncColorPage's sample colour: the user colour, pure black shown white.
    static std::uint32_t sample_rgb(int index) {
        std::uint32_t c = user_color_rgb(index);
        return c == 0 ? 0xffffffu : c;
    }
    int page_ = 0;
    int chat_filter_ = -1; // the chat page's selected filter
    int color_row_ = -1;   // the colours page's selected row
    std::vector<int> color_data_;
    ListData color_list_;
    ComboData chat_choice_;
    ListData chat_list_;
    int key_filter_ = 0; // InitKeyboardPage: SetChoice(0)
    int selected_cmd_ = -1;
    ComboData filter_;
    ListData keys_;
    std::vector<int> commands_;
};

// CBlockedBuffWnd / CBlockedPetBuffWnd (not decoded; this follows their XML over EQEmu's blocked buffs,
// GameConnection::block_buff): the blocked spells' icons (BBuff0..29 / BPBuff0..29, the spell's name as the tooltip;
// a right click unblocks it), a spell name or id typed in the edit box and Add to block it.
class BlockedBuffWindow : public StockWindow {
public:
    BlockedBuffWindow(bool pet)
        : StockWindow(pet ? "BlockedPetBuffWnd" : "BlockedBuffWnd", false), pet_(pet),
          slot_prefix_(pet ? "BPBuff" : "BBuff"), add_(pet ? "BW_AddSpellToBlockFromPet" : "BW_AddSpellToBlock"),
          edit_(pet ? "BW_EditSpellToBlockFromPet" : "BW_EditSpellToBlock") {}

    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        if (id == edit_) {
            out.text = text_;
        } else if (int n = slot(id); n >= 0) {
            const auto& list = host.conn->blocked_buffs(pet_);
            const game::SpellData* s = n < static_cast<int>(list.size())
                                           ? host.conn->spell_table().get(static_cast<std::uint32_t>(list[static_cast<std::size_t>(n)]))
                                           : nullptr;
            out.decal = ChildArt{};
            out.decal.kind = ChildArt::None;
            if (s && host.textures)
                if (auto cell = host.textures->spell_icon(s->spell_icon)) {
                    out.decal.kind = ChildArt::Image;
                    out.decal.cell = *cell;
                }
            out.tooltip = s ? s->name : std::string();
        } else if (id == add_) {
            out.enabled = spell(host) != nullptr;
        }
    }
    bool set_text(EqHost&, std::string_view id, const std::string& text) override {
        if (id != edit_) return false;
        text_ = text;
        return true;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn) return false;
        if (int n = slot(id); n >= 0) {
            const auto& list = host.conn->blocked_buffs(pet_);
            if ((button == "right" || button == "rightup") && n < static_cast<int>(list.size()))
                host.conn->unblock_buff(pet_, list[static_cast<std::size_t>(n)]);
            return true;
        }
        if (id != add_ || button != "left") return false;
        if (const game::SpellData* s = spell(host)) {
            if (!s->beneficial()) host.conn->ui_message(s->name + " is not a beneficial spell."); // this client's line
            else host.conn->block_buff(pet_, static_cast<std::int32_t>(s->id));
            text_.clear();
        }
        return true;
    }

private:
    int slot(std::string_view id) const {
        if (id.rfind(slot_prefix_, 0) != 0 || id.size() == slot_prefix_.size()) return -1;
        int n = 0;
        for (char c : id.substr(slot_prefix_.size())) {
            if (c < '0' || c > '9') return -1;
            n = n * 10 + (c - '0');
        }
        return n < net::kBlockedBuffSlots ? n : -1;
    }
    // The spell the edit box names: an id, or a name as the spell file has it.
    const game::SpellData* spell(EqHost& host) const {
        if (text_.empty()) return nullptr;
        const auto& t = host.conn->spell_table();
        if (text_.find_first_not_of("0123456789") == std::string::npos && text_.size() < 7)
            return t.get(static_cast<std::uint32_t>(std::atoi(text_.c_str())));
        return t.find_by_name(text_);
    }

    bool pet_;
    std::string slot_prefix_, add_, edit_, text_;
};

// CSpellDisplayWnd: SpellDisplayWindow, SpellDisplayWindow#2 .. (CSpellDisplayManager), each over one entry of
// EqHost::spell_displays: the title and the description STML (game/spell_display.h) and the spell's icon.
class SpellDisplayWindow : public StockWindow {
public:
    SpellDisplayWindow(int n, std::string screen) : StockWindow(std::move(screen), false), n_(n) {}

    bool shown(EqHost& host) override { return spell(host) != nullptr; }
    void set_shown(EqHost& host, bool show) override {
        if (!show && n_ < static_cast<int>(host.spell_displays.size()))
            host.spell_displays.erase(host.spell_displays.begin() + n_);
    }
    std::string title(EqHost& host) override {
        const game::SpellDisplay* d = display(host);
        return d ? d->title : std::string();
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        const game::SpellDisplay* d = display(host);
        if (!d) return;
        if (id == "SDW_SpellDescription") {
            out.text = d->description;
        } else if (id == "SDW_IconButton" && host.textures) {
            if (auto cell = host.textures->spell_icon(d->icon)) {
                out.decal.kind = ChildArt::Image;
                out.decal.cell = *cell;
            }
        }
    }

private:
    const game::SpellData* spell(EqHost& host) const {
        if (!host.conn || n_ >= static_cast<int>(host.spell_displays.size())) return nullptr;
        return host.conn->spell_table().get(static_cast<std::uint32_t>(host.spell_displays[static_cast<std::size_t>(n_)].first));
    }
    const game::SpellDisplay* display(EqHost& host) {
        const game::SpellData* s = spell(host);
        if (!s) return nullptr;
        if (!cached_ || cached_id_ != s->id) {
            const auto& c = *host.conn;
            game::SpellDisplayContext ctx;
            ctx.strings = &c.strings();
            ctx.character = host.character;
            ctx.player_level = c.player_status().level;
            ctx.player_class = c.profile() ? static_cast<std::uint8_t>(c.profile()->class_) : 0;
            ctx.db_string = [&c](std::uint32_t id, std::uint32_t type) { return c.strings().db_string(id, type); };
            cached_ = game::build_spell_display(*s, ctx);
            cached_id_ = s->id;
        }
        return &*cached_;
    }

    int n_;
    std::optional<game::SpellDisplay> cached_;
    std::uint32_t cached_id_ = 0;
};


// What a saved report's windows show (BugReportWindow after Send, CrashReportWindow): its file, size and what did
// not fit, and the buttons that open its folder and copy its forum post.
void saved_report_child(const SavedBugReport& saved, std::string_view id, Child& out) {
    if (id == "SavedName") out.text = saved.zip.filename().string();
    else if (id == "SavedFolder") out.text = saved.zip.parent_path().string();
    else if (id == "SavedInfo")
        out.text = saved.size < 1024 * 1024 ? std::format("{} KB", (saved.size + 1023) / 1024)
                                            : std::format("{:.1f} MB", static_cast<double>(saved.size) / (1024.0 * 1024.0));
    else if (id == "SavedLeftOut")
        out.text = saved.left_out.empty() ? std::string()
                                          : std::format("{} file{} left out to stay under Discord's upload limit",
                                                        saved.left_out.size(), saved.left_out.size() == 1 ? "" : "s");
}
bool saved_report_click(const SavedBugReport& saved, std::string_view id) {
    if (id == "OpenFolderButton") platform::open_folder(saved.zip.parent_path());
    else if (id == "CopySummaryButton") ImGui::SetClipboardText(saved.summary.c_str());
    else return false;
    return true;
}

// BugReportWindow (/bugreport; CBugReportWnd is not decoded, this follows its XML over EQEmu's OP_Bug): the category,
// the report, the location (taken when the window opens and on Update Location), the optional flags and the target.
// Send gives the server its copy (OP_Bug) and saves the full report for the player to post (app/bug_report.h): the
// window then shows the saved file (SavedName ...) with Open Folder, Copy Summary and Done (SavedName says it is being
// saved while a worker writes it, so a large log does not stall the frame). One report is written at a time: a Send
// while an earlier one is still being written waits its turn, and the window shows only the last Send since it was
// opened (a report whose window was closed is still written, but not shown). The frame the window
// opens it stays hidden while the report's screenshot is taken, so the picture shows what the player saw.
class BugReportWindow : public StockWindow {
public:
    BugReportWindow() : StockWindow("BugReportWindow", false) {}

    bool shown(EqHost& host) override {
        bool open = host.conn && host.conn->bug_report_open();
        if (open && !was_open_) {
            take_location(host);
            saved_.reset();
            if (host.bug_report_capture) {
                host.bug_report_capture();
                capture_frame_ = ImGui::GetFrameCount();
            }
        }
        if (!open) {
            saved_.reset();
            show_ = 0; // a report still being written is not shown when the window opens again
        }
        // The report Send started, once it is written: nothing written closes the window, as before.
        if (saving_.valid() && saving_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto saved = saving_.get();
            if (open && saving_seq_ == show_) {
                show_ = 0;
                if (saved) saved_ = std::move(saved);
                else if (host.conn) {
                    host.conn->set_bug_report_open(false);
                    open = false;
                }
            }
        }
        if (!saving_.valid() && !queued_.empty()) start_save(host);
        was_open_ = open;
        return open && ImGui::GetFrameCount() != capture_frame_;
    }
    void set_shown(EqHost& host, bool show) override {
        if (host.conn) host.conn->set_bug_report_open(show);
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        if (saved_) saved_report_child(*saved_, id, out);
        else if (show_ != 0 && id == "SavedName") out.text = "(being saved)";
        if (id == "Edit") out.text = text_;
        else if (id == "LocX") out.text = std::format("{:.0f}", loc_.x);
        else if (id == "LocY") out.text = std::format("{:.0f}", loc_.y);
        else if (id == "LocZ") out.text = std::format("{:.0f}", loc_.z);
        else if (id == "TargetName") {
            const auto& w = host.conn->world();
            auto it = w.spawns.find(w.target_id);
            out.text = it == w.spawns.end() ? std::string() : game::display_name(it->second.name);
        } else if (id == "DupButton") out.checked = (flags_ & net::kBugRepeatable) != 0;
        else if (id == "CrashButton") out.checked = (flags_ & net::kBugCrash) != 0;
        else if (id == "TargetButton") out.checked = (flags_ & net::kBugTargetInfo) != 0;
        else if (id == "FlagsButton") out.checked = (flags_ & net::kBugCharacterFlags) != 0;
        else if (id == "SendButton") out.enabled = !text_.empty();
    }
    const ComboData* combo(EqHost&, std::string_view id) override {
        if (id != "BugTypes") return nullptr;
        categories_.choices.clear();
        for (int i = 0; i < net::kBugCategories; ++i) categories_.choices.emplace_back(net::bug_category_name(static_cast<std::uint32_t>(i)));
        if (categories_.selected < 0) categories_.selected = 0;
        return &categories_;
    }
    bool combo_select(EqHost&, std::string_view id, int index) override {
        if (id != "BugTypes") return false;
        categories_.selected = index;
        return true;
    }
    bool set_text(EqHost&, std::string_view id, const std::string& text) override {
        if (id != "Edit") return false;
        text_ = text;
        return true;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        if (saved_ && saved_report_click(*saved_, id)) return true;
        auto toggle = [&](std::uint32_t f) { flags_ ^= f; };
        if (id == "DupButton") toggle(net::kBugRepeatable);
        else if (id == "CrashButton") toggle(net::kBugCrash);
        else if (id == "TargetButton") toggle(net::kBugTargetInfo);
        else if (id == "FlagsButton") toggle(net::kBugCharacterFlags);
        else if (id == "LocationButton") take_location(host);
        else if (id == "SendButton") {
            const auto category = static_cast<std::uint32_t>(std::max(categories_.selected, 0));
            host.conn->send_bug_report(category, flags_, text_);
            saved_.reset();
            if (host.save_bug_report) {
                show_ = ++sends_;
                BugReport r = report(category);
                if (host.bug_report_now) host.bug_report_now(r); // the state at this Send, not at its turn
                queued_.emplace_back(show_, std::move(r));
                if (!saving_.valid()) start_save(host);
            }
            text_.clear();
            flags_ = 0;
            if (show_ == 0) host.conn->set_bug_report_open(false);
        } else if (id == "CancelButton" || id == "DoneButton") host.conn->set_bug_report_open(false);
        else return false;
        return true;
    }

private:
    // The next queued report to the worker; one that cannot be started counts as not written.
    void start_save(EqHost& host) {
        while (!queued_.empty() && !saving_.valid()) {
            auto [seq, r] = std::move(queued_.front());
            queued_.pop_front();
            if (host.save_bug_report) saving_ = host.save_bug_report(std::move(r));
            saving_seq_ = seq;
            if (!saving_.valid() && seq == show_) {
                show_ = 0;
                if (host.conn) host.conn->set_bug_report_open(false);
            }
        }
    }
    void take_location(EqHost& host) {
        const game::Spawn* me = host.conn ? host.conn->world().player() : nullptr;
        if (me) loc_ = game::world_to_server(me->position);
    }
    BugReport report(std::uint32_t category) const {
        BugReport r;
        r.category = net::bug_category_name(category);
        static constexpr std::pair<std::uint32_t, const char*> kFlags[] = {
            {net::kBugRepeatable, "Can be repeated"}, {net::kBugCrash, "Crash"},
            {net::kBugCharacterFlags, "Character flags"}, {net::kBugTargetInfo, "Target info"}};
        for (const auto& [bit, name] : kFlags)
            if (flags_ & bit) r.flags.emplace_back(name);
        r.text = text_;
        r.facts.emplace_back("Reported at", std::format("{:.0f}, {:.0f}, {:.0f}", loc_.x, loc_.y, loc_.z));
        return r;
    }
    ComboData categories_;
    std::string text_;
    std::uint32_t flags_ = 0;
    glm::vec3 loc_{0.0f};
    bool was_open_ = false;
    int capture_frame_ = -1;
    std::optional<SavedBugReport> saved_;
    std::future<std::optional<SavedBugReport>> saving_; // a Send's report, while it is written
    std::uint32_t saving_seq_ = 0;                       // which Send saving_ is
    std::deque<std::pair<std::uint32_t, BugReport>> queued_; // Sends waiting for saving_
    std::uint32_t sends_ = 0;                            // Sends so far
    std::uint32_t show_ = 0; // the Send whose file the window waits to show; 0 for none
};

// CrashReportWindow (Hailstone's own): shown at the start after a crash, over any screen, with the report the client
// made from it (app/bug_report.h): its file, a note the player can add (Edit, SaveNoteButton: the report is written
// again with it), Open Folder, Copy Summary and Close.
class CrashReportWindow : public StockWindow {
public:
    CrashReportWindow() : StockWindow("CrashReportWindow", false) {}
    bool pre_game() const override { return true; }
    bool shown(EqHost& host) override { return host.crash_report != nullptr; }
    void set_shown(EqHost& host, bool show) override {
        if (!show && host.crash_report_close) host.crash_report_close();
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.crash_report) return;
        saved_report_child(*host.crash_report, id, out);
        if (id == "Edit") out.text = note_;
        else if (id == "SaveNoteButton") out.enabled = !note_.empty() && note_ != saved_note_;
        else if (id == "NoteSaved") out.text = saved_note_.empty() ? std::string() : "Note saved in the report";
    }
    bool set_text(EqHost&, std::string_view id, const std::string& text) override {
        if (id != "Edit") return false;
        note_ = text;
        return true;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.crash_report || button != "left") return false;
        if (saved_report_click(*host.crash_report, id)) return true;
        if (id == "SaveNoteButton") {
            if (host.crash_report_note) host.crash_report_note(note_);
            saved_note_ = note_;
        } else if (id == "CloseButton") {
            if (host.crash_report_close) host.crash_report_close();
        } else return false;
        return true;
    }

private:
    std::string note_, saved_note_;
};

// FeedbackWindow (/feedback; CFeedbackWnd): the character's name (FBKW_UserName, set
// by the constructor), the text (FBKW_FeedbackInput, at most 0x3ff characters); Send sends it with the position and
// heading (OP_Feedback) when there is text and clears it, Cancel clears it; both close the window.
class FeedbackWindow : public StockWindow {
public:
    FeedbackWindow() : StockWindow("FeedbackWindow", false) {}
    bool shown(EqHost& host) override { return host.conn && host.conn->feedback_open(); }
    void set_shown(EqHost& host, bool show) override {
        if (host.conn) host.conn->set_feedback_open(show);
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        if (id == "FBKW_UserName") out.text = host.conn->character_name();
        else if (id == "FBKW_FeedbackInput") out.text = text_;
    }
    bool set_text(EqHost&, std::string_view id, const std::string& text) override {
        if (id != "FBKW_FeedbackInput") return false;
        text_ = text.substr(0, 0x3ff);
        return true;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        if (id == "FBKW_SendButton") {
            host.conn->send_feedback(text_);
            if (!text_.empty()) text_.clear();
        } else if (id == "FBKW_CancelButton") {
            text_.clear();
            host.conn->set_feedback_open(false);
        } else return false;
        return true;
    }

private:
    std::string text_;
};

// AlarmWnd (/alarm, CMD_TOGGLE_ALARM; CAlarmWnd is not decoded): hours and minutes, Set Time starts the alarm
// (GameConnection::set_alarm), Skip closes the window.
class AlarmWindow : public StockWindow {
public:
    AlarmWindow() : StockWindow("AlarmWnd", false) {}
    bool shown(EqHost& host) override { return host.conn && host.conn->alarm_open(); }
    void set_shown(EqHost& host, bool show) override {
        if (host.conn) host.conn->set_alarm_open(show);
    }
    void child(EqHost&, std::string_view id, Child& out) override {
        if (id == "ALRW_HoursEditbox") out.text = hours_;
        else if (id == "ALRW_MinutesEditbox") out.text = minutes_;
    }
    bool set_text(EqHost&, std::string_view id, const std::string& text) override {
        if (id == "ALRW_HoursEditbox") hours_ = text;
        else if (id == "ALRW_MinutesEditbox") minutes_ = text;
        else return false;
        return true;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        if (id == "ALRW_OkButton") {
            std::int64_t ms = (std::max(0, std::atoi(hours_.c_str())) * 60LL + std::max(0, std::atoi(minutes_.c_str()))) * 60000;
            host.conn->set_alarm(ms);
            host.conn->set_alarm_open(false);
        } else if (id == "ALRW_CancelButton") {
            host.conn->set_alarm_open(false);
        } else return false;
        return true;
    }

private:
    std::string hours_ = "0", minutes_ = "0";
};

// ClaimWnd (/claim; not decoded, its XML over EQEmu's veteran rewards): the rewards the account may claim, the
// selected one's items, Refresh, Claim and Cancel.
class ClaimWindow : public StockWindow {
public:
    ClaimWindow() : StockWindow("ClaimWnd", false) {}

    bool shown(EqHost& host) override { return host.conn && host.conn->claims().open; }
    void set_shown(EqHost& host, bool show) override {
        if (host.conn && show != host.conn->claims().open) host.conn->set_claims_open(show);
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        if (host.conn && id == "ClaimClaimButton") {
            const auto* r = selected(host);
            out.enabled = r && r->enabled && r->available > 0;
        }
    }
    const ListData* list(EqHost& host, std::string_view id) override {
        if (!host.conn) return nullptr;
        const auto& rewards = host.conn->claims().rewards;
        if (id == "ClaimFeatureList") {
            features_ = {};
            features_.columns = 2;
            features_.labels = {"Reward", "Available"};
            features_.widths = {220, 70};
            for (std::size_t i = 0; i < rewards.size(); ++i) {
                const auto& r = rewards[i];
                std::string name = r.items.empty() ? std::format("Reward {}", r.claim_id) : r.items.front().name;
                features_.rows.push_back(ListRow{{name, std::to_string(r.available)}});
                if (!r.enabled || r.available == 0) features_.rows.back().color = Rgb{0.6f, 0.6f, 0.6f};
            }
            features_.selected = selected_ < static_cast<int>(rewards.size()) ? selected_ : -1;
            return &features_;
        }
        if (id == "ClaimItemList") {
            items_ = {};
            items_.columns = 2;
            items_.labels = {"Item", "Charges"};
            items_.widths = {230, 60};
            if (const auto* r = selected(host))
                for (const auto& it : r->items) items_.rows.push_back(ListRow{{it.name, std::to_string(it.charges)}});
            return &items_;
        }
        return nullptr;
    }
    bool list_click(EqHost&, std::string_view id, int row, bool, int) override {
        if (id == "ClaimFeatureList") selected_ = row;
        return id == "ClaimFeatureList" || id == "ClaimItemList";
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        if (id == "ClaimRefreshButton") host.conn->claims_refresh();
        else if (id == "ClaimClaimButton") {
            if (const auto* r = selected(host)) host.conn->claim_reward(r->claim_id);
        } else if (id == "ClaimCancelButton") host.conn->set_claims_open(false);
        else return false;
        return true;
    }

private:
    const net::VetReward* selected(EqHost& host) const {
        const auto& r = host.conn->claims().rewards;
        return selected_ >= 0 && selected_ < static_cast<int>(r.size()) ? &r[static_cast<std::size_t>(selected_)] : nullptr;
    }
    ListData features_, items_;
    int selected_ = -1;
};

} // namespace

void open_quantity(EqHost& host, const game::QuantityOpen& in, std::function<void(std::int32_t)> done,
                   std::function<bool()> owner_valid) {
    if (owner_valid) {
        done = [valid = owner_valid, accept = std::move(done)](std::int32_t n) {
            if (valid() && accept) accept(n);
        };
    }
    game::QuantityOpen q = in;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    q.screen_width = static_cast<std::int32_t>(ds.x);
    q.screen_height = static_cast<std::int32_t>(ds.y);
    if (auto r = host.textures ? host.textures->screen_rect("QuantityWnd") : std::nullopt) {
        q.rect[0] = 0, q.rect[1] = 0, q.rect[2] = r->w, q.rect[3] = r->h;
    }
    const ImGuiIO& io = ImGui::GetIO();
    q.shift = io.KeyShift;
    q.ctrl = io.KeyCtrl;
    game::QuantityResult r = game::quantity_dialog_open(q);
    if (r.closed) {
        host.quantity.reset();
        return;
    }
    if (r.accepted) {
        host.quantity.reset();
        if (done) done(r.quantity);
        return;
    }
    EqHost::UiQuantity u;
    u.min = q.min;
    u.max = q.max;
    u.slider = r.slider;
    u.position = r.position;
    u.done = std::move(done);
    u.owner_valid = std::move(owner_valid);
    u.serial = ++host.quantity_serial;
    host.quantity = std::move(u);
}

void add_dialog_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<BugReportWindow>());
    out.push_back(std::make_unique<CrashReportWindow>());
    out.push_back(std::make_unique<FeedbackWindow>());
    out.push_back(std::make_unique<ClaimWindow>());
    out.push_back(std::make_unique<AlarmWindow>());
    out.push_back(std::make_unique<BlockedBuffWindow>(false));
    out.push_back(std::make_unique<BlockedBuffWindow>(true));
    out.push_back(std::make_unique<QuantityWindow>());
    out.push_back(std::make_unique<OptionsWindow>());
    for (int n = 0; n < 6; ++n)
        out.push_back(std::make_unique<SpellDisplayWindow>(
            n, n == 0 ? std::string("SpellDisplayWindow") : "SpellDisplayWindow#" + std::to_string(n + 1)));
}

} // namespace hailstone::app
