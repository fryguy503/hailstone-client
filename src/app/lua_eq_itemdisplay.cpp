// The item display windows (ItemDisplayWindow, ItemDisplayWindow#2 ..): CItemDisplayWnd over game/item_display.h.
// The text is what CItemDisplayWnd_BuildItemInfo builds; the places of the info lines, the stat grid, the heroic
// bonuses, the Modified button and the description are those it gives them (0x69b590 and the end of
// BuildItemInfo), so the window is drawn "live" (eq.ChildRect), at the size it gives itself there
// (CXWnd::Resize @006adce8: eq.WindowClient; game::layout_item_display). The augment sockets are
// CItemDisplayWnd_UpdateAugmentSlots @0x69f240's rows (game/augments.h) and their input CItemDisplayWnd::
// WndNotification @006ae870's.

#include <cfloat>
#include <charconv>
#include <cmath>
#include <string>

#include <imgui.h>

#include "app/lua_eq.h"
#include "app/lua_eq_windows.h"
#include "core/log.h"
#include "game/augments.h"
#include "game/inventory.h"
#include "game/item_display.h"
#include "game/item_links.h"
#include "game/mechanics/character.h"
#include "game/server_ext.h"
#include "game/spells.h"
#include "net/game_connection.h"
#include "ui/lua/lua_host.h"

namespace hailstone::app {

namespace {

// "IDW_Row12Col3Value" -> {12, 3, "Value"}
bool parse_cell(std::string_view id, int& row, int& col, std::string_view& part) {
    if (!id.starts_with("IDW_Row")) return false;
    id.remove_prefix(7);
    auto c = id.find("Col");
    if (c == std::string_view::npos) return false;
    if (std::from_chars(id.data(), id.data() + c, row).ec != std::errc()) return false;
    id.remove_prefix(c + 3);
    std::size_t n = 0;
    while (n < id.size() && id[n] >= '0' && id[n] <= '9') ++n;
    if (std::from_chars(id.data(), id.data() + n, col).ec != std::errc()) return false;
    part = id.substr(n);
    return row >= 1 && row <= game::ItemDisplay::kRows && col >= 1 && col <= 3;
}

int index_after(std::string_view id, std::string_view prefix) {
    if (!id.starts_with(prefix)) return -1;
    int n = 0;
    auto r = std::from_chars(id.data() + prefix.size(), id.data() + id.size(), n);
    return r.ec == std::errc() && r.ptr == id.data() + id.size() ? n : -1;
}


class ItemDisplayWindow : public StockWindow {
public:
    ItemDisplayWindow(int n, std::string screen) : StockWindow(std::move(screen), false), n_(n) {}

    bool shown(EqHost& host) override { return item(host) != nullptr; }
    void set_shown(EqHost& host, bool show) override {
        if (!show && host.item_displays && n_ < static_cast<int>(host.item_displays->size()))
            host.close_item_display(n_);
    }
    std::string title(EqHost& host) override {
        const game::ItemDisplay* d = display(host);
        if (d) return d->title;
        const net::Item* it = item(host);
        return it ? it->name : std::string();
    }

    // SetItem @006ae100 when the window shows another item, and UpdateAugmentSlots for the cursor of this frame
    // (the original refreshes the rows whenever the cursor attachment changes).
    void frame(EqHost& host) override {
        const net::Item* it = item(host);
        if (!it || !host.conn) {
            shown_id_ = 0;
            layout_.reset();
            return;
        }
        std::uint64_t key = window_key(host);
        if (key != shown_key_ || it->id != shown_id_) {
            // SolventText is cleared, and for an augment with a SolventItemID OP_AugmentInfo asks for its name
            // (@006ae4b2) with the window's index.
            shown_key_ = key;
            shown_id_ = it->id;
            // SetItem's BuildItemInfo(item, flags): CItemDisplayManager::ShowItem passes !Shift, which this client's
            // inspect does not have, so 1: the window lays out and sizes itself again.
            build_flags_ = 1;
            if (it->item_type == game::kItemTypeAugment && it->aug_distiller != 0)
                host.conn->request_augment_info(it->aug_distiller, static_cast<std::uint32_t>(key));
            else host.conn->request_augment_info(0, static_cast<std::uint32_t>(key));
        }
        game::AugmentStrings as;
        as.strings = &host.conn->strings();
        const net::GameConnection* c = host.conn;
        as.db_string = [c](std::uint32_t id, std::uint32_t type) { return c->strings().db_string(id, type); };
        const game::AugmentContext ac = host.conn->augment_context();
        sockets_ = game::build_augment_sockets(*it, host.conn->inventory().cursor(), as, ac.appearance_feature,
                                               ac.worn_classes);
    }

    bool stml_link(EqHost& host, std::string_view id, int message, std::string_view data) override {
        // XWM_LINK (27) from the window's STML boxes (the socket rows' augment names): "a^b"; a = 0 shows child
        // b's display (b < 6). (a = 1, Send_OP_EvolveItem, is for evolving items, not kept here.)
        (void)id;
        if (message != 27) return false;
        auto caret = data.find('^');
        if (caret == std::string_view::npos) return false;
        int a = -1, b = -1;
        std::from_chars(data.data(), data.data() + caret, a);
        std::from_chars(data.data() + caret + 1, data.data() + data.size(), b);
        if (a == 0 && b >= 0 && b < game::kAugSockets) show_augment(host, b, false);
        return true;
    }

    void child(EqHost& host, std::string_view id, Child& out) override {
        const game::ItemDisplay* d = display(host);
        if (!d) return;
        if (int n = index_after(id, "IDW_ItemInfo"); n >= 1) {
            if (n > static_cast<int>(d->info.size())) {
                out.shown = false;
                return;
            }
            const auto& line = d->info[static_cast<std::size_t>(n - 1)];
            out.shown = true;
            out.text = line.text;
            out.color = argb_to_rgb(line.argb);
            return;
        }
        int row = 0, col = 0;
        std::string_view part;
        if (parse_cell(id, row, col, part)) {
            const auto& c = d->grid[static_cast<std::size_t>(row - 1)][static_cast<std::size_t>(col - 1)];
            out.shown = c.shown;
            if (part == "Stat") {
                out.text = c.stat;
                if (!c.tooltip.empty()) out.tooltip = c.tooltip;
            } else {
                out.text = c.value;
                out.color = argb_to_rgb(c.value_argb);
            }
            return;
        }
        if (int n = index_after(id, "IDW_Heroic"); n >= 1) {
            const auto* c = heroic(*d, n);
            out.shown = c != nullptr;
            if (c) {
                out.text = c->heroic;
                out.color = argb_to_rgb(game::kHeroicArgb);
            }
            return;
        }
        if (id == "ItemDescription") {
            // UpdateStrings @0x69ae30: ItemInfo, then SolventText and "<BR>" when there is one.
            out.text = d->description;
            std::string solvent = host.conn ? host.conn->augment_solvent_text(static_cast<std::uint32_t>(window_key(host)))
                                            : std::string();
            if (!solvent.empty()) *out.text += solvent + "<BR>";
            return;
        }
        if (int n = socket_of(id); n >= 0) {
            const game::AugmentSocketRow& socket_row = sockets_.rows[static_cast<std::size_t>(n)];
            socket_child(host, id, socket_row, socket_row.shown, out);
            return;
        }
        if (id.starts_with("IDW_Appearance_Socket")) {
            if (id == "IDW_Appearance_Socket_Buy_Button") {
                out.shown = sockets_.appearance_shown && sockets_.buy_shown;
                out.tooltip = sockets_.buy_tooltip;
                return;
            }
            socket_child(host, id, sockets_.appearance, sockets_.appearance_shown, out);
            return;
        }
        if (id == "IDW_ModButtonLabel") {
            out.text = d->mod_button;
            return;
        }
        if (id == "IconButton") {
            const net::Item* it = item(host);
            if (it && host.textures)
                if (auto cell = host.textures->item_icon(it->icon)) {
                    out.decal.kind = ChildArt::Image;
                    out.decal.cell = *cell;
                }
            return;
        }
        // The maker and the lore tab: not kept for items here.
        if (id == "IDW_MadeBy") out.shown = false;
    }

    std::optional<std::array<int, 4>> child_rect(EqHost& host, std::string_view id) override {
        const game::ItemDisplay* d = display(host);
        const game::ItemDisplayLayout* l = layout(host);
        if (!d || !l) return std::nullopt;
        if (id == "IDW_Appearance_Socket_Screen") return std::array<int, 4>{0, l->appearance_top, 700, 40};
        int row = 0, col = 0;
        std::string_view part;
        if (parse_cell(id, row, col, part)) {
            static constexpr int kX[3] = {10, 140, 260}, kW[3] = {110, 100, 100};
            return std::array<int, 4>{kX[col - 1], l->row_top(row), kW[col - 1], 20};
        }
        if (int n = index_after(id, "IDW_Heroic"); n >= 1) {
            // 0x69b3d0: right of the value it belongs to, 40 wide.
            int k = 0;
            for (int r = 1; r <= game::ItemDisplay::kRows; ++r)
                for (int c = 1; c <= 3; ++c)
                    if (!d->grid[static_cast<std::size_t>(r - 1)][static_cast<std::size_t>(c - 1)].heroic.empty() && ++k == n) {
                        static constexpr int kRight[3] = {120, 240, 360};
                        return std::array<int, 4>{kRight[c - 1], l->row_top(r), 40, 20};
                    }
            return std::nullopt;
        }
        // The Modified button and its label (@006adcf4: 10 right of the button, 100 wide, as high).
        if (id == "IDW_ModButton") return std::array<int, 4>{10, l->mod_top, 15, 15};
        if (id == "IDW_ModButtonLabel") return std::array<int, 4>{35, l->mod_top, 100, 15};
        if (int i = socket_of(id); i >= 0 && id.ends_with("_Screen")) {
            int top = l->socket_top[static_cast<std::size_t>(i)];
            if (top >= 0) return std::array<int, 4>{0, top, 700, 20};
            return std::nullopt;
        }
        // Anchored to the page's right and bottom (child_anchor): the window's size gives its height, and the STML
        // scrolls.
        if (id == "ItemDescription") return std::array<int, 4>{10, l->description_top, 0, 0};
        return std::nullopt;
    }

    std::optional<std::array<int, 2>> child_anchor(EqHost& host, std::string_view id) override {
        // CXWnd_SetAnchors(10, 1, top, 1, 0, 0, 3, 0) @006adc4a: right 0 and bottom 3 inside the page.
        if (id == "ItemDescription" && layout(host)) return std::array<int, 2>{0, 3};
        return std::nullopt;
    }

    std::optional<std::array<int, 2>> window_size(EqHost& host) override {
        if (!layout(host)) return std::nullopt;
        return size_;
    }
    void size_window(EqHost& host, int w, int h) override {
        // The user's resizing: the children keep their places, the anchored ones stretch (CItemDisplayWnd has no
        // OnResize).
        if (layout(host)) size_ = {w, h};
    }

    bool click(EqHost& host, std::string_view id, std::string_view button, bool outside) override {
        if (id == "IDW_ModButton" && button == "left") {
            // WndNotification @006ae945: the Modified view, then BuildItemInfo(item, 0) @006ae95b.
            modified_ = !modified_;
            cached_ = {};
            build_flags_ = 0;
            return true;
        }
        if (id == "IconButton" && button == "left") {
            // WndNotification @006ae870 (IconButton): the item as a link into the active chat window's input, which takes the
            // focus (game::display_icon_links). One link through Link All's path adds nothing to a full input.
            const net::Item* it = item(host);
            if (it && host.conn && game::display_icon_links(*it))
                request_chat_link_all({game::make_item_link(*it, host.conn->server_ext().wide_item_links(*host.conn))});
            return true;
        }
        if (id == "IDW_Appearance_Socket_Buy_Button")
            return button == "left"; // 0x6dcc00(0x432) opens the marketplace, which this client does not have
        if (button == "rightup" && !outside && ImGui::GetIO().KeyAlt) {
            // XWM_RCLICK with Alt on any control: Send_OP_ItemPreviewRequest(the window's item) and the preview shown.
            if (host.conn) host.conn->preview_item(item(host));
            return true;
        }
        int socket = -1;
        if (int n = socket_of(id); n >= 0 && id.ends_with("_Item")) socket = n;
        else if (id == "IDW_Appearance_Socket_Item") socket = sockets_.appearance_socket;
        if (socket < 0) return false;
        const net::Item* it = item(host);
        if (!it || !host.conn) return false;
        const ImGuiIO& io = ImGui::GetIO();
        if (button == "left") {
            // XWM_LCLICK: Alt shows the augment (sub_006ae7b0), else insert / remove / swap (sub_006ae010).
            if (io.KeyAlt) show_augment(host, socket, false);
            else host.conn->augment_click(*it, socket);
            return true;
        }
        if (button == "rightheld") { // XWM_RCLICKHOLD (24): the augment's display while the button is held
            show_augment(host, socket, true);
            return true;
        }
        if (button == "rightupheld") { // XWM_RBUTTONUPAFTERHELD (26): CWndDisplayManager_CloseNewest
            close_held(host);
            return true;
        }
        if (button == "rightup" && !outside) {
            // XWM_RCLICK without Alt: the socket's menu (sub_006a01b0), at the mouse (Alt is the preview, above).
            net::Item window_item = *it;
            std::vector<game::AugmentMenuRow> rows =
                game::augment_socket_menu(window_item, socket, host.conn->augment_context(), &host.conn->strings());
            if (rows.empty()) return true;
            std::vector<EqHost::MenuRow> menu;
            for (const auto& r : rows) {
                EqHost::MenuRow m;
                m.text = r.text;
                m.rgb = r.argb & 0xffffff;
                m.enabled = r.enabled;
                if (r.enabled) {
                    std::uint32_t menu_id = r.id;
                    // The connection as it is when the row is chosen: a camp or logout in between ends it.
                    EqHost* hp = &host;
                    m.act = [hp, window_item, socket, menu_id] {
                        if (hp->conn) hp->conn->augment_menu_select(window_item, socket, menu_id);
                    };
                }
                menu.push_back(std::move(m));
            }
            ImVec2 mouse = ImGui::GetMousePos();
            if (!ImGui::IsMousePosValid(&mouse)) mouse = {host.click_rect[0] > 0 ? host.click_rect[0] : 100.0f,
                                                          host.click_rect[1] > 0 ? host.click_rect[1] : 100.0f};
            host.open_menu(mouse.x, mouse.y, std::move(menu));
            return true;
        }
        return button == "rightdown";
    }

private:
    // "IDW_Socket_Slot_3_Item" -> 2.
    static int socket_of(std::string_view id) {
        if (!id.starts_with("IDW_Socket_Slot_") || id.size() < 17) return -1;
        int n = id[16] - '1';
        return n >= 0 && n < game::kAugSockets ? n : -1;
    }

    // A socket screen's children: the screen shown, the button's decal (the augment's icon) and tooltip, the STML.
    void socket_child(EqHost& host, std::string_view id, const game::AugmentSocketRow& row, bool shown, Child& out) {
        if (id.ends_with("_Screen")) {
            out.shown = shown;
        } else if (id.ends_with("_Item")) {
            out.tooltip = row.tooltip;
            out.decal.kind = ChildArt::None;
            if (row.filled && host.textures)
                if (auto cell = host.textures->item_icon(row.icon)) {
                    out.decal.kind = ChildArt::Image;
                    out.decal.cell = *cell;
                }
        } else if (id.ends_with("_Description")) {
            out.text = row.text;
        }
    }

    // The window's own number (ItemWndIndex), kept while older displays close.
    std::uint64_t window_key(EqHost& host) const {
        if (!host.item_display_ids || n_ >= static_cast<int>(host.item_display_ids->size())) return 0;
        return (*host.item_display_ids)[static_cast<std::size_t>(n_)];
    }

    // sub_006ae7b0 @006ae7b0: CItemDisplayManager::ShowItem(the socket's augment), nothing for an empty socket. The
    // augment's index is the host's with the socket as its augment slot.
    void show_augment(EqHost& host, int socket, bool held) {
        const net::Item* it = item(host);
        const net::Item* aug = it ? game::augment_in(*it, socket) : nullptr;
        if (!aug || !host.inspect_item) return;
        net::Item copy = *aug;
        copy.slot = it->slot;
        copy.slot.aug = static_cast<std::int16_t>(socket);
        held_id_ = held ? copy.id : 0;
        host.inspect_item(copy);
    }
    void close_held(EqHost& host) {
        if (held_id_ == 0 || !host.item_displays || !host.close_item_display) return;
        for (int i = static_cast<int>(host.item_displays->size()) - 1; i >= 0; --i)
            if ((*host.item_displays)[static_cast<std::size_t>(i)].id == held_id_) {
                host.close_item_display(i);
                break;
            }
        held_id_ = 0;
    }

    // Hailstone: "Hailstone_Item", the shown item for eq.SlotItem and eq.CompareSlotItem (a child of its own: as an
    // item slot the icon would take the slot clicks of eq.ClickChild).
    const net::Item* slot_item(EqHost& host, std::string_view id) override {
        return id == "Hailstone_Item" ? item(host) : nullptr;
    }

    const net::Item* item(EqHost& host) const {
        if (!host.item_displays || n_ >= static_cast<int>(host.item_displays->size())) return nullptr;
        return &(*host.item_displays)[static_cast<std::size_t>(n_)];
    }

    // The display, built again when the item or the frame's state changes (once a second at most otherwise).
    const game::ItemDisplay* display(EqHost& host) {
        const net::Item* it = item(host);
        if (!it || !host.conn) return nullptr;
        auto now = net::GameConnection::now_ms();
        // The augments are part of the key: a refreshed item (same serial) with a new augment builds again at once.
        std::string sig = it->serial;
        for (const auto& c : it->contents) sig += "/" + std::to_string(c.index) + ":" + std::to_string(c.item.id);
        if (cached_ && cached_id_ == it->id && cached_serial_ == sig && now - built_ms_ < 1000) return &*cached_;
        const net::GameConnection& c = *host.conn;
        // BuildItemInfo's 0x7b28a0 on the shown item (the stats-disabled bytes: game/augments.h), with
        // GetGameFeature(UseItem) and GetGameFeature(Ornament); the item's header slot is its ItemGlobalIndex.
        net::Item& shown = (*host.item_displays)[static_cast<std::size_t>(n_)];
        game::update_stats_disabled(shown, c.game_feature(0x13), c.game_feature(0x15),
                                    shown.slot.slot != -1 && shown.slot.sub == -1);
        game::ItemDisplayContext ctx;
        ctx.strings = &c.strings();
        ctx.character = host.character;
        ctx.player_level = c.player_status().level;
        ctx.player_class = c.profile() ? static_cast<std::uint8_t>(c.profile()->class_) : 0;
        ctx.player_classes = c.player_class_mask();
        ctx.modified = modified_;
        ctx.heading = host.in_game ? host.heading : -1.0f; // a compass item's direction
        const game::mechanics::Character* ch = host.character;
        std::uint32_t classes = ctx.player_classes;
        // A server's client may answer CanUseItem / CanEquipItem true for everything and write its own "Level
        // Needed" line (the NMS patch: "Level Needed: CLR(1) ...").
        const bool all = c.server_ext().can_use_all_items(c);
        ctx.can_use_all = all;
        ctx.level_needed_line = c.server_ext().level_needed_line(c);
        ctx.can_equip = [ch, classes, all](const net::Item& i) {
            if (!all && !game::can_use_class(i, classes)) return false;
            if (!all && ch && !ch->can_use_item(i, false)) return false;
            for (int s = 0; s < 23; ++s)
                if ((i.equip_slots & (1u << s)) && game::can_go_in_equip_slot(i, s, nullptr, nullptr, true)) return true;
            return false;
        };
        ctx.deity_name = [&c](std::uint32_t deity) { return deity_desc(c, deity); };
        ctx.spell = [&c](std::int32_t id) { return c.spell_table().get(static_cast<std::uint32_t>(id)); };
        ctx.db_string = [&c](std::uint32_t id, std::uint32_t type) { return c.strings().db_string(id, type); };
        cached_ = game::build_item_display(*it, ctx);
        cached_id_ = it->id;
        cached_serial_ = sig;
        built_ms_ = now;
        return &*cached_;
    }

    // The layout BuildItemInfo last gave the window, done again for a pending BuildItemInfo(item, flags). 0x69b590
    // always places the grid again; with flags 0 (@006ad921) the rest only moves, and the window only resizes, when
    // the Modified button's row has come below the description's top (it grew).
    const game::ItemDisplayLayout* layout(EqHost& host) {
        const game::ItemDisplay* d = display(host);
        if (!d) return nullptr;
        if (build_flags_ >= 0 || !layout_) {
            game::ItemDisplayFrameIn in;
            in.appearance = sockets_.appearance_shown;
            in.appearance_text = static_cast<int>(sockets_.appearance.text.size());
            for (std::size_t i = 0; i < in.sockets.size(); ++i) {
                in.sockets[i] = sockets_.rows[i].shown;
                in.socket_text[i] = static_cast<int>(sockets_.rows[i].text.size());
            }
            in.text_width = game::item_display_text_width(*d, system_font_extent);
            game::ItemDisplayLayout fresh = game::layout_item_display(*d, in);
            const int description_top = layout_ ? layout_->description_top : 0; // the XML's TopAnchorOffset
            if (build_flags_ != 0 || description_top < fresh.mod_top) {
                layout_ = fresh;
                size_ = {fresh.width, fresh.height};
                log::info("item display {}: {} x {}", screen_, size_[0], size_[1]);
            } else {
                layout_->grid_top = fresh.grid_top;
                layout_->appearance_top = fresh.appearance_top;
            }
            build_flags_ = -1;
        }
        return &*layout_;
    }

    // CTextureFont::GetTextExtent of pSystemFont (FontsArray[2], 12 pixels; lua_eq_map.cpp's kFontPixels), in the
    // original's pixels.
    static int system_font_extent(std::string_view s) {
        ImFont* f = ui::eq_font(2);
        if (!f || !ImGui::GetCurrentContext()) return static_cast<int>(s.size()) * 6;
        float scale = f->FontSize / 12.0f;
        return static_cast<int>(std::lround(f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, s.data(), s.data() + s.size()).x / scale));
    }

    static const game::ItemDisplayCell* heroic(const game::ItemDisplay& d, int n) {
        int k = 0;
        for (const auto& row : d.grid)
            for (const auto& c : row)
                if (!c.heroic.empty() && ++k == n) return &c;
        return nullptr;
    }

    int n_;
    bool modified_ = false;
    game::AugmentSockets sockets_;
    std::uint64_t shown_key_ = 0;
    std::uint32_t shown_id_ = 0;
    std::uint32_t held_id_ = 0; // the augment display a held right button opened
    std::optional<game::ItemDisplay> cached_;
    std::optional<game::ItemDisplayLayout> layout_;
    std::array<int, 2> size_{};
    int build_flags_ = -1; // a BuildItemInfo to do: its flags (1 SetItem, 0 the Modified button), -1 none
    std::uint32_t cached_id_ = 0;
    std::string cached_serial_;
    std::int64_t built_ms_ = 0;
};

} // namespace

std::string deity_desc(const net::GameConnection& c, std::uint32_t deity) {
    // CEverQuest::GetDeityDesc: the eqstr id of each deity (the same table as the inventory's deity label).
    static constexpr std::pair<std::uint32_t, std::uint32_t> kDeity[] = {
        {0xc9, 0xcb3}, {0xca, 0xcbe}, {0xcb, 0xcb5}, {0xcc, 0xcb8}, {0xcd, 0xcb4}, {0xce, 0xcb6}, {0xcf, 0xcb7},
        {0xd0, 0xcb9}, {0xd1, 0xcbb}, {0xd2, 0xcbc}, {0xd3, 0xcc2}, {0xd4, 0xcba}, {0xd5, 0xcbd}, {0xd6, 0xcbf},
        {0xd7, 0xcc0}, {0xd8, 0xcc1}};
    std::uint32_t id = 0xcb2;
    for (auto [d, s] : kDeity)
        if (d == deity) id = s;
    return c.strings().format(id, {});
}

// ---------------------------------------------------------------- ObjectPreviewWnd (CObjectPreviewWnd)

// CObjectPreviewWnd @0x7005a0 over GameConnection::item_preview() (game/item_preview.h): shown by a preview request
// (and its close box: AboutToHide stops the view), hidden on zoning (OnPreZone). The reply's 0x6ff980 shows the
// view (OPW_ViewRenderArea, app/item_preview_view.h) and its eleven buttons when there is something to show, else
// the "No item preview available." label. WndNotification @0x6ffb20 hands the buttons to the view's controller
// (0x5770f0's object, not decoded): here the pan buttons move the view a tenth of its size, the rotate buttons
// turn it 15 degrees, the zoom buttons scale the distance by 0.8 / 1.25, Reset fits it again; the Lua drags turn and
// pan it and the wheel zooms it, as the buttons' tooltips say.
class ObjectPreviewWindow : public StockWindow {
public:
    ObjectPreviewWindow() : StockWindow("ObjectPreviewWnd", false) {}

    bool shown(EqHost& host) override { return host.conn && host.conn->item_preview().open; }
    void set_shown(EqHost& host, bool show) override {
        if (host.conn) host.conn->set_item_preview_open(show);
    }
    std::string title(EqHost& host) override {
        const auto* p = host.conn ? &host.conn->item_preview() : nullptr;
        return p && p->item && p->show ? p->item->name : std::string();
    }
    void child(EqHost& host, std::string_view id, Child& out) override {
        const bool show = host.conn && host.conn->item_preview().show;
        if (id == "OPW_NoItemPreviewAvailable_Label") out.shown = !show;
        else if (id.starts_with("OPW_")) out.shown = show;
    }
    bool click(EqHost& host, std::string_view id, std::string_view button, bool) override {
        if (button != "left" || !host.item_preview_control) return false;
        constexpr float kTurn = 0.2617994f; // 15 degrees
        static constexpr struct { std::string_view id, what; float x, y; } kButtons[] = {
            {"OPW_PanLeftButton", "pan", -0.1f, 0.0f},      {"OPW_PanRightButton", "pan", 0.1f, 0.0f},
            {"OPW_PanUpButton", "pan", 0.0f, 0.1f},         {"OPW_PanDownButton", "pan", 0.0f, -0.1f},
            {"OPW_RotateLeftButton", "rotate", -kTurn, 0.0f}, {"OPW_RotateRightButton", "rotate", kTurn, 0.0f},
            {"OPW_RotateUpButton", "rotate", 0.0f, kTurn},  {"OPW_RotateDownButton", "rotate", 0.0f, -kTurn},
            {"OPW_ZoomInButton", "zoom", 0.8f, 0.0f},       {"OPW_ZoomOutButton", "zoom", 1.25f, 0.0f},
            {"OPW_ResetButton", "reset", 0.0f, 0.0f},
        };
        for (const auto& b : kButtons)
            if (id == b.id) {
                host.item_preview_control(b.what, b.x, b.y, 0);
                return true;
            }
        return false;
    }
};

void add_item_display_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    out.push_back(std::make_unique<ObjectPreviewWindow>());
    for (int n = 0; n < kItemDisplayWindows; ++n)
        out.push_back(std::make_unique<ItemDisplayWindow>(
            n, n == 0 ? std::string("ItemDisplayWindow") : "ItemDisplayWindow#" + std::to_string(n + 1)));
}

} // namespace hailstone::app
