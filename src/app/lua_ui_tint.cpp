// BodyTintWnd and ColorPickerWnd. CBodyTintWnd is not decoded; it follows its XML over EQEmu's OP_Dye
// (GameConnection::dye_armor). The tint window shows each armour slot's colour now and the new one; a slot button
// paints the brush on it, the brush button opens the colour picker, Save dyes the changed slots (a Vial of Prismatic
// Dye each) and Reset forgets the changes. The colour picker is CColorPickerWnd's, opened by any caller through
// UiHost::pick_color (the tint window's brush, the map toolbar's colour buttons).

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>

#include "app/lua_ui_windows.h"
#include "net/game_connection.h"
#include "net/player_packets.h"

namespace hailstone::app {

namespace {

constexpr int kTintSlots = 7; // head, chest, arms, wrist, hands, legs, feet (the weapons are not tintable)
constexpr const char* kSlotNames[kTintSlots] = {"Head", "Chest", "Arms", "ForeArm", "Hands", "Legs", "Feet"};

struct TintState {
    std::array<std::uint32_t, kTintSlots> edited{};
    std::array<bool, kTintSlots> changed{};
    std::uint32_t brush = 0xffffff; // 0xRRGGBB
};

std::array<float, 4> rgba(std::uint32_t rgb) {
    auto c = [&](int shift) { return static_cast<float>((rgb >> shift) & 0xff) / 255.0f; };
    return {c(16), c(8), c(0), 1.0f};
}

// The player's tint of a slot now (its wear changes), 0xRRGGBB; untinted reads as white.
std::uint32_t current_tint(UiHost& host, int slot) {
    const game::Spawn* me = host.conn ? host.conn->world().player() : nullptr;
    if (!me) return 0xffffff;
    std::uint32_t t = me->appearance.tint[static_cast<std::size_t>(slot)];
    return (t >> 24) ? (t & 0xffffff) : 0xffffff;
}

// The slot a TINT_<Name>Btn (new) or TINT_O<Name>ColorBtn (now) names, else -1.
int slot_of(std::string_view id, bool original) {
    for (int i = 0; i < kTintSlots; ++i) {
        std::string want = original ? std::format("TINT_O{}ColorBtn", kSlotNames[i]) : std::format("TINT_{}Btn", kSlotNames[i]);
        if (id == want) return i;
    }
    return -1;
}

class BodyTintWindow : public StockWindow {
public:
    explicit BodyTintWindow(std::shared_ptr<TintState> s) : StockWindow("BodyTintWnd", false), s_(std::move(s)) {}

    bool shown(UiHost& host) override { return host.body_tint_open && host.in_game; }
    void set_shown(UiHost& host, bool show) override {
        host.body_tint_open = show;
        if (!show) reset(host);
    }

    void child(UiHost& host, std::string_view id, Child& out) override {
        if (!host.conn) return;
        if (int n = slot_of(id, true); n >= 0) {
            out.tint = rgba(current_tint(host, n));
        } else if (int m = slot_of(id, false); m >= 0) {
            out.tint = rgba(s_->changed[static_cast<std::size_t>(m)] ? s_->edited[static_cast<std::size_t>(m)] : current_tint(host, m));
        } else if (id == "TINT_CurBrushBtn") {
            out.tint = rgba(s_->brush);
        } else if (id == "TINT_ReagentsNeeded") {
            out.text = std::format("Reagents Needed: {}", needed(host));
        } else if (id == "TINT_ReagentsLabel") {
            out.text = std::format("Reagents Available: {}", host.conn->prismatic_dye_count());
        } else if (id == "TINT_SaveTint") {
            int k = needed(host);
            out.enabled = k > 0 && static_cast<std::uint32_t>(k) <= host.conn->prismatic_dye_count();
        }
    }

    bool click(UiHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.conn || button != "left") return false;
        if (int m = slot_of(id, false); m >= 0) {
            s_->edited[static_cast<std::size_t>(m)] = s_->brush;
            s_->changed[static_cast<std::size_t>(m)] = s_->brush != current_tint(host, m);
        } else if (int n = slot_of(id, true); n >= 0) {
            s_->brush = current_tint(host, n); // the colour now becomes the brush
        } else if (id == "TINT_CurBrushBtn") {
            auto st = s_;
            host.pick_color(s_->brush, [st](std::uint32_t argb) { st->brush = argb & 0xffffff; }, this);
        } else if (id == "TINT_ResetBodytTint") {
            reset(host);
        } else if (id == "TINT_SaveTint") {
            const game::Spawn* me = host.conn->world().player();
            if (!me) return true;
            std::array<std::uint32_t, 9> tints = me->appearance.tint;
            for (int i = 0; i < kTintSlots; ++i)
                if (s_->changed[static_cast<std::size_t>(i)]) tints[static_cast<std::size_t>(i)] = 0xff000000u | s_->edited[static_cast<std::size_t>(i)];
            host.conn->dye_armor(tints);
            reset(host);
        } else {
            return false;
        }
        return true;
    }

private:
    int needed(UiHost& host) const {
        int n = 0;
        for (int i = 0; i < kTintSlots; ++i)
            if (s_->changed[static_cast<std::size_t>(i)] && s_->edited[static_cast<std::size_t>(i)] != current_tint(host, i)) ++n;
        return n;
    }
    void reset(UiHost& host) {
        s_->changed = {};
        if (host.color_pick && host.color_pick->caller == this) host.color_pick.reset(); // the brush's picker closes
    }

    std::shared_ptr<TintState> s_;
};

// CColorPickerWnd. Open @0x659af0: the caller, the colour into the three
// values, sliders (0..255, SetNumTicks(0x100)) and edit boxes ("%d", numbers only, 3 characters), the swatches'
// decal tints back to their XML colours (CRNormal) and CPW_TC15Button's to the colour (sub_006595d0).
// WndNotification @0x6596b0: a slider moved writes its edit box; an edit box changed reads all three (atoi, above
// 255 written back as 255; sub_00658e90); a swatch 0..14 takes its tint; Accept sends the caller notification 0x25
// with 0xffRRGGBB and closes; the close box closes (AboutToHide forgets the caller).
class ColorPickerWindow : public StockWindow {
public:
    ColorPickerWindow() : StockWindow("ColorPickerWnd", false) {}

    bool shown(UiHost& host) override {
        if (host.color_pick && host.color_pick->serial != serial_) {
            serial_ = host.color_pick->serial;
            set_rgb(host.color_pick->rgb);
        }
        return host.color_pick.has_value();
    }
    void set_shown(UiHost& host, bool show) override {
        if (!show) host.color_pick.reset();
    }
    void child(UiHost& host, std::string_view id, Child& out) override {
        if (!host.color_pick) return;
        if (int n = swatch(id); n >= 0) {
            std::uint32_t c = n == 15 ? rgb() : kSwatches[static_cast<std::size_t>(n)];
            out.tint = std::array<float, 4>{static_cast<float>((c >> 16) & 0xff) / 255.0f,
                                            static_cast<float>((c >> 8) & 0xff) / 255.0f,
                                            static_cast<float>(c & 0xff) / 255.0f, 1.0f};
            return;
        }
        if (int* v = value_of(id, "Slider")) out.slider_value = *v, out.slider_count = 0x100;
        else if (int k = edit_of(id); k >= 0) out.text = edits_[static_cast<std::size_t>(k)];
    }
    bool set_slider(UiHost& host, std::string_view id, int value) override {
        int* v = host.color_pick ? value_of(id, "Slider") : nullptr;
        if (!v) return false;
        *v = std::clamp(value, 0, 255);
        sync_edits(); // UpdateEditWndFromSlider
        return true;
    }
    bool set_text(UiHost& host, std::string_view id, const std::string& text) override {
        int k = host.color_pick ? edit_of(id) : -1;
        if (k < 0) return false;
        // EditMode 3 (digits), MaxChars 3; then sub_00658e90: atoi, clamped to MaxSliderValue (255) and written back.
        std::string digits;
        for (char c : text)
            if (c >= '0' && c <= '9' && digits.size() < 3) digits += c;
        int v = std::atoi(digits.c_str());
        if (v > 255) {
            v = 255;
            digits = "255";
        }
        edits_[static_cast<std::size_t>(k)] = digits;
        values_[static_cast<std::size_t>(k)] = v;
        return true;
    }
    bool click(UiHost& host, std::string_view id, std::string_view button, bool) override {
        if (!host.color_pick || button != "left") return false;
        if (id == "CPW_Accept_Button") {
            auto done = std::move(host.color_pick->done);
            std::uint32_t argb = 0xff000000u | rgb();
            host.color_pick.reset();
            if (done) done(argb);
            return true;
        }
        int n = swatch(id);
        if (n < 0) return false;
        if (n < 15) set_rgb(kSwatches[static_cast<std::size_t>(n)]);
        return true;
    }

private:
    // The swatches' XML colours (EQUI_ColorPickerWnd.xml, CPW_TC0Button .. CPW_TC14Button).
    static constexpr std::array<std::uint32_t, 15> kSwatches{0xf00000, 0x00f000, 0x0000f0, 0xf0f000, 0xf0f0f0,
                                                             0x7f0000, 0x007f00, 0x00007f, 0x7f7f00, 0x7f7f7f,
                                                             0x00f0f0, 0x007f7f, 0xf07f00, 0x7f4000, 0x000000};
    static int swatch(std::string_view id) {
        if (id.size() < 13 || id.substr(0, 6) != "CPW_TC" || id.substr(id.size() - 6) != "Button") return -1;
        auto num = id.substr(6, id.size() - 12);
        int n = 0;
        for (char c : num) {
            if (c < '0' || c > '9') return -1;
            n = n * 10 + (c - '0');
        }
        return num.empty() || n > 15 ? -1 : n;
    }
    int* value_of(std::string_view id, std::string_view suffix) {
        static constexpr std::array<std::string_view, 3> kNames{"CPW_Red", "CPW_Green", "CPW_Blue"};
        for (std::size_t i = 0; i < 3; ++i)
            if (id.size() == kNames[i].size() + suffix.size() && id.substr(0, kNames[i].size()) == kNames[i] &&
                id.substr(kNames[i].size()) == suffix)
                return &values_[i];
        return nullptr;
    }
    int edit_of(std::string_view id) {
        int* v = value_of(id, "SliderInput");
        return v ? static_cast<int>(v - values_.data()) : -1;
    }
    std::uint32_t rgb() const {
        return static_cast<std::uint32_t>(values_[0] & 0xff) << 16 | static_cast<std::uint32_t>(values_[1] & 0xff) << 8 |
               static_cast<std::uint32_t>(values_[2] & 0xff);
    }
    void set_rgb(std::uint32_t c) {
        values_ = {static_cast<int>((c >> 16) & 0xff), static_cast<int>((c >> 8) & 0xff), static_cast<int>(c & 0xff)};
        sync_edits();
    }
    void sync_edits() {
        for (std::size_t i = 0; i < 3; ++i) edits_[i] = std::to_string(values_[i]);
    }

    int serial_ = 0;
    std::array<int, 3> values_{};
    std::array<std::string, 3> edits_;
};

} // namespace

void add_tint_windows(std::vector<std::unique_ptr<StockWindow>>& out) {
    auto s = std::make_shared<TintState>();
    out.push_back(std::make_unique<BodyTintWindow>(s));
    out.push_back(std::make_unique<ColorPickerWindow>());
}

} // namespace hailstone::app
