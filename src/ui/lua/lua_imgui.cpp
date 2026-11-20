// The `ui` Lua table: Dear ImGui for scripts. Functions keep ImGui's names; values that ImGui changes through
// pointers are returned instead (value, changed = ui.Checkbox("x", value)). Colours are r, g, b, a in 0..1.
// Textures (for example item icons from hs.ItemIcon) are light userdata.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h" // stack sizes, to refuse pops and ends that ImGui would only assert on
#include "ui/imgui_layer.h"
#include "ui/lua/lua_host.h"
#include "ui/texture_ids.h"
#include "core/lua_keep.h"

#include <lua.hpp>

namespace hailstone::ui::lua {

// ui/lua/lua_code_editor.cpp
int CodeEditor(lua_State* L);
int SetClipboardText(lua_State* L);
int GetClipboardText(lua_State* L);

namespace {

// A double beyond float's range is undefined behaviour to convert: it is held to +-FLT_MAX (NaN and the infinities
// convert as they are).
float to_float(double v) {
	constexpr double kMax = FLT_MAX;
	return static_cast<float>(std::isfinite(v) ? std::clamp(v, -kMax, kMax) : v);
}
float F(lua_State *L, int i, float def = 0.0f) { return to_float(luaL_optnumber(L, i, def)); }
int I(lua_State *L, int i, int def = 0) { return static_cast<int>(luaL_optinteger(L, i, def)); }
const char *S(lua_State *L, int i) { return luaL_checkstring(L, i); }
bool B(lua_State *L, int i, bool def = false) { return lua_isnoneornil(L, i) ? def : lua_toboolean(L, i) != 0; }
ImVec4 Color(lua_State *L, int i) { return ImVec4(F(L, i, 1), F(L, i + 1, 1), F(L, i + 2, 1), F(L, i + 3, 1)); }
int Ret(lua_State *L, bool v) { lua_pushboolean(L, v); return 1; }

// --- the plugin consent question's windows (lua_host.h set_consent_guard) ---
bool g_guard = false;
std::string g_guardLuaWindow; // the pack's Lua window that asked hs.PluginConsent this frame
int g_guardLuaFrame = -1;
std::vector<ImGuiID> g_guarded, g_guardedBefore; // this frame's guarded ImGui windows, and last frame's
int g_guardedFrame = -1;

void guarded_frame() {
	if (g_guardedFrame == ImGui::GetFrameCount()) return;
	g_guardedBefore = std::move(g_guarded);
	g_guarded.clear();
	g_guardedFrame = ImGui::GetFrameCount();
}
void guard(ImGuiWindow *w) {
	if (!w) return;
	guarded_frame();
	if (std::find(g_guarded.begin(), g_guarded.end(), w->ID) == g_guarded.end()) g_guarded.push_back(w->ID);
}
bool guarded(const ImGuiWindow *w) {
	if (!w || !g_guard) return false;
	guarded_frame();
	auto has = [&](const std::vector<ImGuiID> &v) { return std::find(v.begin(), v.end(), w->ID) != v.end(); };
	return has(g_guarded) || has(g_guardedBefore);
}

// --- windows ---
// A script's window flags: ImGui's public ones. From 1 << 24 they are its own (ChildWindow, Tooltip, Popup, Modal,
// ChildMenu: "Don't use!"); Popup without an open popup indexes OpenPopupStack out of bounds.
int WindowFlags(lua_State *L, int i) { return I(L, i) & ((1 << 24) - 1); }
// The windows scripts began with ui.Begin and ui.BeginChild this frame, innermost last: ui.End and ui.EndChild close
// only the window on top, and only one of the matching kind, so a script cannot end a window it did not begin (the
// implicit Debug window, a C++ window it was called from, a popup's or a child's with the wrong call). A refused
// Begin (a guarded window, below) is kept as an entry that is no window: its End does nothing.
struct Begun {
	ImGuiWindow *window; // nullptr: refused, nothing was begun
	bool child;
	int clips = 0;
};
std::vector<Begun> g_begun;
std::vector<ImGuiTabBar*> g_tabItems;
enum class PopupKind { Popup, Combo, Menu, Tooltip };
struct PopupBegin { ImGuiWindow* window; PopupKind kind; };
std::vector<PopupBegin> g_popups;
ImGuiContext *g_begunContext = nullptr;
int g_begunFrame = -1;
std::vector<Begun> &begun() {
	if (g_begunContext != GImGui || g_begunFrame != ImGui::GetFrameCount()) {
		g_begun.clear();
		g_tabItems.clear();
		g_popups.clear();
		g_begunContext = GImGui;
		g_begunFrame = ImGui::GetFrameCount();
	}
	return g_begun;
}
// The ImGui stacks' sizes as each script's draw function began (windows_begun), innermost last: its PopStyleColor,
// PopStyleVar and PopFont pop only what it pushed, never what the C++ code that drew around it pushed.
struct Floor {
	ImGuiErrorRecoveryState stacks;
	ImGuiWindow* window = nullptr;
	ImDrawList* clipList = nullptr;
	ImDrawList* trackedClipList = nullptr;
	std::size_t begun = 0, tabItems = 0, popupRecords = 0;
	int tables = 0, tabs = 0, combos = 0, wraps = 0, clips = 0;
	int trackedClipFrame = -1, trackedClipDepth = 0, foregroundFrame = -1;
	float fontScale = 1;
	bool preserveParent = false;
	ImGuiNextWindowData nextWindow;
	ImGuiNextItemData nextItem;
	ImGuiLastItemData lastItem;
};
std::vector<Floor> g_floors;
ImDrawList *g_clipList = nullptr;
int g_clipFrame = -1, g_clipDepth = 0;
extern int g_foregroundFrame;
int PoppableColors() { return GImGui->ColorStack.Size - (g_floors.empty() ? 0 : g_floors.back().stacks.SizeOfColorStack); }
int PoppableVars() { return GImGui->StyleVarStack.Size - (g_floors.empty() ? 0 : g_floors.back().stacks.SizeOfStyleVarStack); }
int PoppableFonts() { return GImGui->FontStack.Size - (g_floors.empty() ? 0 : g_floors.back().stacks.SizeOfFontStack); }
bool OwnWindow() { return g_floors.empty() || GImGui->CurrentWindowStack.Size > g_floors.back().stacks.SizeOfWindowStack; }
bool FloorWindow() { return !g_floors.empty() && GImGui->CurrentWindow == g_floors.back().window; }
bool RememberPopup(bool opened, PopupKind kind) {
	if (opened) { begun(); g_popups.push_back({GImGui->CurrentWindow, kind}); }
	return opened;
}
bool OwnPopup(PopupKind kind) {
	begun();
	return OwnWindow() && !g_popups.empty() && g_popups.size() > (g_floors.empty() ? 0 : g_floors.back().popupRecords) &&
		g_popups.back().window == GImGui->CurrentWindow && g_popups.back().kind == kind;
}

// Whether code that is not the pack's may not draw into `w` (or a child of it): it shows the consent question.
bool refused_to(lua_State *L, const ImGuiWindow *w) {
	if (!w || !g_guard || pack_code(L)) return false;
	return guarded(w) || (w->RootWindow && w->RootWindow != w && guarded(w->RootWindow));
}
// The client's own consent prompt (app/automation.cpp), refused by name too: before its first frame it is not
// guarded yet, and a script drawing first would otherwise create it.
bool consent_window_name(const char *name) {
	return ImHashStr(name) == ImHashStr(kConsentWindowName);
}
// A refused Begin leaves nothing behind for the next window: the script's SetNextWindow* calls are dropped, so they
// cannot move or resize the window begun next (the consent prompt, when it follows).
void refuse_begin(bool child) {
	GImGui->NextWindowData.ClearFlags();
	begun().push_back({nullptr, child});
}

// visible, open = ui.Begin(title [, open [, flags [, collapseRight]]])   open: nil = no close button;
// collapseRight: the title bar's collapse (roll-up) button at its right, beside the close button (ImGui's
// WindowMenuButtonPosition, for this window alone), not at its left. While the plugin consent question is up, code
// that is not the pack's cannot begin its windows (visible is false, and its ui.End does nothing).
int Begin(lua_State *L) {
	const char *name = S(L, 1);
	bool hasOpen = !lua_isnoneornil(L, 2);
	bool open = B(L, 2, true);
	const int flags = WindowFlags(L, 3);
	if (g_guard && !pack_code(L) && (consent_window_name(name) || refused_to(L, ImGui::FindWindowByName(name)))) {
		refuse_begin(false);
		lua_pushboolean(L, false);
		lua_pushboolean(L, open);
		return 2;
	}
	ImGuiStyle &style = ImGui::GetStyle();
	const ImGuiDir menuButton = style.WindowMenuButtonPosition;
	if (lua_toboolean(L, 4)) style.WindowMenuButtonPosition = ImGuiDir_Right;
	bool visible = ImGui::Begin(name, hasOpen ? &open : nullptr, flags);
	style.WindowMenuButtonPosition = menuButton;
	begun().push_back({ImGui::GetCurrentWindow(), false, ImGui::GetWindowDrawList()->_ClipRectStack.Size});
	if (g_guard && g_guardLuaFrame == ImGui::GetFrameCount() && LuaUi::drawing_window() == g_guardLuaWindow &&
		pack_code(L))
		guard(ImGui::GetCurrentWindow());
	lua_pushboolean(L, visible);
	lua_pushboolean(L, open);
	return 2;
}
// Closes the window on top of g_begun when it is the current one and of the kind asked.
void end_begun(lua_State *L, bool child) {
	std::vector<Begun> &b = begun();
	if (!g_floors.empty() && b.size() <= g_floors.back().begun) return;
	if (b.empty() || b.back().child != child) return;
	if (!b.back().window) { // a refused Begin: nothing to end
		b.pop_back();
		return;
	}
	ImGuiWindow *w = GImGui->CurrentWindow;
	if (!OwnWindow()) return;
	if (!w || w != b.back().window || GImGui->CurrentWindowStack.Size <= 1) return;
	if (((w->Flags & ImGuiWindowFlags_ChildWindow) != 0) != child) return;
	if (w->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip)) return; // EndPopup's, EndTooltip's
	if (refused_to(L, w)) return;
	while (w->DrawList->_ClipRectStack.Size > b.back().clips) w->DrawList->PopClipRect();
	b.pop_back();
	if (child)
		ImGui::EndChild();
	else
		ImGui::End();
}
int End(lua_State *L) { end_begun(L, false); return 0; }
// visible = ui.BeginChild(id [, w, h [, border [, flags]]]) ... ui.EndChild()   (EndChild whatever it returned).
// Refused, as Begin is, in a window of the consent question for code that is not the pack's.
int BeginChild(lua_State *L) {
	const char *id = S(L, 1);
	const ImVec2 size(F(L, 2), F(L, 3));
	const ImGuiChildFlags childFlags = B(L, 4) ? ImGuiChildFlags_Borders : 0;
	const int flags = WindowFlags(L, 5);
	if (refused_to(L, GImGui->CurrentWindow)) {
		refuse_begin(true);
		return Ret(L, false);
	}
	const bool visible = ImGui::BeginChild(id, size, childFlags, flags);
	begun().push_back({ImGui::GetCurrentWindow(), true, ImGui::GetWindowDrawList()->_ClipRectStack.Size});
	return Ret(L, visible);
}
int EndChild(lua_State *L) { end_begun(L, true); return 0; }
int SetNextWindowSize(lua_State *L) { ImGui::SetNextWindowSize(ImVec2(F(L, 1), F(L, 2)), I(L, 3, ImGuiCond_FirstUseEver)); return 0; }
int SetNextWindowSizeConstraints(lua_State *L) {
	ImGui::SetNextWindowSizeConstraints(ImVec2(F(L, 1), F(L, 2)), ImVec2(F(L, 3), F(L, 4)));
	return 0;
}
int SetNextWindowPos(lua_State *L) { ImGui::SetNextWindowPos(ImVec2(F(L, 1), F(L, 2)), I(L, 3, ImGuiCond_FirstUseEver)); return 0; }
// ui.SetNextWindowCollapsed(collapsed [, cond])   rolled up to its title bar (cond: ui.Cond, default Always)
int SetNextWindowCollapsed(lua_State *L) {
	int cond = I(L, 2, ImGuiCond_Always);
	if (cond <= 0 || (cond & (cond - 1)) != 0 || cond > ImGuiCond_Appearing) cond = ImGuiCond_Always; // (ImGui asserts on more than one)
	ImGui::SetNextWindowCollapsed(B(L, 1), cond);
	return 0;
}
// ui.IsWindowCollapsed() -> the current window is rolled up to its title bar
int IsWindowCollapsed(lua_State *L) { return Ret(L, ImGui::IsWindowCollapsed()); }
int SetNextWindowBgAlpha(lua_State *L) { ImGui::SetNextWindowBgAlpha(F(L, 1)); return 0; }
int SetWindowFontScale(lua_State *L) { ImGui::SetWindowFontScale(F(L, 1, 1)); return 0; }

// --- text ---
int Text(lua_State *L) { ImGui::TextUnformatted(S(L, 1)); return 0; }
int TextColored(lua_State *L) { ImGui::TextColored(Color(L, 1), "%s", S(L, 5)); return 0; }
int TextDisabled(lua_State *L) { ImGui::TextDisabled("%s", S(L, 1)); return 0; }
int TextWrapped(lua_State *L) { ImGui::TextWrapped("%s", S(L, 1)); return 0; }
// ui.PushTextWrapPos([x])   0: wrap at the window's edge / ui.PopTextWrapPos()
int PushTextWrapPos(lua_State *L) { ImGui::PushTextWrapPos(F(L, 1, 0)); return 0; }
int PopTextWrapPos(lua_State *) {
	if (GImGui->CurrentWindow && GImGui->CurrentWindow->DC.TextWrapPosStack.Size > (FloorWindow() ? g_floors.back().wraps : 0)) {
		ImGui::PopTextWrapPos();
	}
	return 0;
}
int SeparatorText(lua_State *L) { ImGui::SeparatorText(S(L, 1)); return 0; }
int LabelText(lua_State *L) { ImGui::LabelText(S(L, 1), "%s", S(L, 2)); return 0; }
int BulletText(lua_State *L) { ImGui::BulletText("%s", S(L, 1)); return 0; }
// w, h = ui.CalcTextSize(text)
int CalcTextSize(lua_State *L) {
	ImVec2 v = ImGui::CalcTextSize(S(L, 1));
	lua_pushnumber(L, v.x);
	lua_pushnumber(L, v.y);
	return 2;
}

// --- widgets ---
int Button(lua_State *L) { return Ret(L, ImGui::Button(S(L, 1), ImVec2(F(L, 2), F(L, 3)))); }
int SmallButton(lua_State *L) { return Ret(L, ImGui::SmallButton(S(L, 1))); }
// ui.InvisibleButton(id, w, h [, flags])   -- flags ui.ButtonFlags: EnableNav makes it reachable by the gamepad's
// navigation (ImGui 1.91 leaves invisible buttons out of it by default); MouseButtonLeft + MouseButtonRight make
// either button press it (a drag with either keeps it active)
int InvisibleButton(lua_State *L) { return Ret(L, ImGui::InvisibleButton(S(L, 1), ImVec2(F(L, 2, 1), F(L, 3, 1)), I(L, 4, 0))); }
// ui.EdgeButton(id, x0, y0, x1, y1) -> pressed: an invisible button at that screen rectangle that takes the mouse up
// to the window's outer edge (an item is otherwise hit only inside the window's padding): a window's own resize
// handles.
int EdgeButton(lua_State *L) {
	ImGuiWindow *w = ImGui::GetCurrentWindow();
	ImGui::PushClipRect(w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y), false);
	ImGui::SetCursorScreenPos(ImVec2(F(L, 2), F(L, 3)));
	ImGui::SetNextItemAllowOverlap();
	bool pressed = ImGui::InvisibleButton(S(L, 1), ImVec2(std::max(F(L, 4) - F(L, 2), 1.0f), std::max(F(L, 5) - F(L, 3), 1.0f)));
	ImGui::PopClipRect();
	return Ret(L, pressed);
}
int Selectable(lua_State *L) { return Ret(L, ImGui::Selectable(S(L, 1), B(L, 2), I(L, 3), ImVec2(F(L, 4), F(L, 5)))); }
int Checkbox(lua_State *L) {
	bool v = B(L, 2);
	bool changed = ImGui::Checkbox(S(L, 1), &v);
	lua_pushboolean(L, v);
	lua_pushboolean(L, changed);
	return 2;
}
// Colour widgets accept normalized RGB only. Keep invalid numbers and assertion-prone flag combinations out
// of ImGui; a Lua argument error is raised before any widget opens its own groups, popup or ID stack.
void ColorFrame(lua_State *L) {
	if (!GImGui || !GImGui->WithinFrameScope || !GImGui->CurrentWindow)
		luaL_error(L, "colour widgets require an active ImGui frame");
}
float ColorComponent(lua_State *L, int at, float fallback = 0.0f) {
	const double value = luaL_optnumber(L, at, fallback);
	if (!std::isfinite(value)) luaL_argerror(L, at, "finite colour component expected");
	return static_cast<float>(std::clamp(value, 0.0, 1.0));
}
float ColorButtonSize(lua_State *L, int at) {
	const double value = luaL_optnumber(L, at, 0.0);
	if (!std::isfinite(value)) luaL_argerror(L, at, "finite colour button size expected");
	return static_cast<float>(std::clamp(value, 0.0, 4096.0));
}
int ColorWidgetFlags(lua_State *L, int at, bool picker, bool button = false) {
	constexpr int kCommon = ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoOptions |
		ImGuiColorEditFlags_NoSmallPreview | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoTooltip |
		ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoDragDrop |
		ImGuiColorEditFlags_NoBorder | ImGuiColorEditFlags_AlphaOpaque | ImGuiColorEditFlags_AlphaNoBg |
		ImGuiColorEditFlags_AlphaPreviewHalf;
	constexpr int kOptions = ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_DisplayHSV |
		ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_Uint8 | ImGuiColorEditFlags_Float |
		ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_InputRGB;
	constexpr int kButton = ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop |
		ImGuiColorEditFlags_NoBorder | ImGuiColorEditFlags_AlphaOpaque | ImGuiColorEditFlags_AlphaNoBg |
		ImGuiColorEditFlags_AlphaPreviewHalf;
	if (!lua_isnoneornil(L, at) && lua_type(L, at) != LUA_TNUMBER)
		luaL_argerror(L, at, "integer colour flags expected");
	const lua_Integer raw = luaL_optinteger(L, at, 0);
	if (raw < 0) luaL_argerror(L, at, "nonnegative colour flags expected");
	int flags = static_cast<int>(static_cast<lua_Unsigned>(raw) & static_cast<lua_Unsigned>(button ? kButton : kCommon | kOptions));
	if (button) return flags;
	// No HDR or HSV input: callers always receive finite RGB in 0..1. The editor cannot change global options.
	flags |= ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_InputRGB | ImGuiColorEditFlags_NoOptions;
	const int picker_flags = flags & ImGuiColorEditFlags_PickerMask_;
	if (picker_flags != ImGuiColorEditFlags_PickerHueBar && picker_flags != ImGuiColorEditFlags_PickerHueWheel)
		flags = (flags & ~ImGuiColorEditFlags_PickerMask_) | ImGuiColorEditFlags_PickerHueWheel;
	const int data_flags = flags & ImGuiColorEditFlags_DataTypeMask_;
	if (data_flags != ImGuiColorEditFlags_Uint8 && data_flags != ImGuiColorEditFlags_Float)
		flags = (flags & ~ImGuiColorEditFlags_DataTypeMask_) | ImGuiColorEditFlags_Uint8;
	const int display_flags = flags & ImGuiColorEditFlags_DisplayMask_;
	if (display_flags == 0 || (!picker && (display_flags & (display_flags - 1)) != 0))
		flags = (flags & ~ImGuiColorEditFlags_DisplayMask_) | ImGuiColorEditFlags_DisplayRGB;
	return flags;
}
// pressed = ui.ColorButton(id, r, g, b [, a=1 [, flags=0 [, w=0, h=0]]])
int ColorButton(lua_State *L) {
	ColorFrame(L);
	const char *label = S(L, 1);
	const ImVec4 color(ColorComponent(L, 2), ColorComponent(L, 3), ColorComponent(L, 4), ColorComponent(L, 5, 1));
	const int flags = ColorWidgetFlags(L, 6, false, true);
	const ImVec2 size(ColorButtonSize(L, 7), ColorButtonSize(L, 8));
	return Ret(L, ImGui::ColorButton(label, color, flags, size));
}
int ColorRgbWidget(lua_State *L, bool picker) {
	ColorFrame(L);
	const char *label = S(L, 1);
	float color[] = {ColorComponent(L, 2), ColorComponent(L, 3), ColorComponent(L, 4)};
	const int flags = ColorWidgetFlags(L, 5, picker);
	const bool changed = picker ? ImGui::ColorPicker3(label, color, flags) : ImGui::ColorEdit3(label, color, flags);
	for (const float component : color) lua_pushnumber(L, std::isfinite(component) ? std::clamp(component, 0.0f, 1.0f) : 0.0f);
	lua_pushboolean(L, changed);
	return 4;
}
// r, g, b, changed = ui.ColorPicker3(label, r, g, b [, flags=0])
int ColorPicker3(lua_State *L) { return ColorRgbWidget(L, true); }
int ColorEdit3(lua_State *L) { return ColorRgbWidget(L, false); }
int SliderFloat(lua_State *L) {
	float v = F(L, 2);
	bool changed = ImGui::SliderFloat(S(L, 1), &v, F(L, 3), F(L, 4, 1));
	lua_pushnumber(L, v);
	lua_pushboolean(L, changed);
	return 2;
}
// value, changed = ui.SliderInt(label, value, min, max [, text])   text: shown on the slider instead of the value
int SliderInt(lua_State *L) {
	const char *label = S(L, 1); // every argument read before the format string exists (a Lua error skips its destructor)
	int v = I(L, 2);
	const int lo = I(L, 3), hi = I(L, 4, 100);
	const char *given = lua_type(L, 5) == LUA_TSTRING ? lua_tostring(L, 5) : nullptr;
	std::string text;
	if (given) {
		for (const char *c = given; *c; ++c) {
			if (*c == '%') text += '%';
			text += *c;
		}
	}
	bool changed = ImGui::SliderInt(label, &v, lo, hi, given ? text.c_str() : "%d");
	lua_pushinteger(L, v);
	lua_pushboolean(L, changed);
	return 2;
}
// text, changed = ui.InputText(label, text [, maxLength [, flags [, options]]])
// options: history = function(older) -> text, called on Up (older true) / Down to replace the text;
// tab = function(text, shift) -> text, called on Tab (shift: Shift+Tab) with what the input holds, to replace it
// (nil leaves it); moveToEnd = true puts the text cursor at the end (instead of selecting everything when focused
// from code).
struct InputTextOptions {
	lua_State *L;
	int history; // stack index of the history function, 0 for none
	int tab;     // stack index of the tab function, 0 for none
	bool moveToEnd;
};
// The tab function (1) with the text being edited (2: the callback's data) and Shift, inside the callback's
// protected call: pushing the text can raise (the state's memory limit), and nothing may raise across ImGui's frames.
int TabCall(lua_State *L) {
	const auto *d = static_cast<const ImGuiInputTextCallbackData *>(lua_touserdata(L, 2));
	lua_settop(L, 1);
	lua_pushlstring(L, d->Buf, static_cast<size_t>(d->BufTextLen));
	lua_pushboolean(L, ImGui::GetIO().KeyShift);
	lua_call(L, 2, 1);
	return 1;
}
int InputTextCallback(ImGuiInputTextCallbackData *d) {
	auto *o = static_cast<InputTextOptions *>(d->UserData);
	if (d->EventFlag == ImGuiInputTextFlags_CallbackHistory && o->history) {
		lua_pushvalue(o->L, o->history);
		lua_pushboolean(o->L, d->EventKey == ImGuiKey_UpArrow);
		if (lua_pcall(o->L, 1, 1, 0) == LUA_OK && lua_type(o->L, -1) == LUA_TSTRING) {
			d->DeleteChars(0, d->BufTextLen);
			d->InsertChars(0, lua_tostring(o->L, -1));
		}
		lua_pop(o->L, 1); // the result or the error
	}
	else if (d->EventFlag == ImGuiInputTextFlags_CallbackCompletion && o->tab) {
		lua_pushcfunction(o->L, &TabCall);
		lua_pushvalue(o->L, o->tab);
		lua_pushlightuserdata(o->L, d);
		if (lua_pcall(o->L, 2, 1, 0) == LUA_OK && lua_type(o->L, -1) == LUA_TSTRING) {
			d->DeleteChars(0, d->BufTextLen);
			d->InsertChars(0, lua_tostring(o->L, -1));
		}
		lua_pop(o->L, 1); // the result or the error
	}
	else if (d->EventFlag == ImGuiInputTextFlags_CallbackAlways && o->moveToEnd) {
		d->CursorPos = d->SelectionStart = d->SelectionEnd = d->BufTextLen;
	}
	return 0;
}
// The buffer ImGui edits in place: the text, cut to `cap` bytes, and room up to cap with a NUL ending it inside the
// buf_size ImGui is given (cap + 1), however long the text was.
std::string TextBuffer(const char *cur, size_t len, int cap) {
	std::string buf(cur, std::min(len, static_cast<size_t>(cap)));
	buf.resize(static_cast<size_t>(cap) + 1);
	buf[static_cast<size_t>(cap)] = '\0';
	return buf;
}
// Pushes the edited buffer's text: moved first into a string that outlives a push that raises (the state's memory
// limit), which would skip the caller's destructors (core/lua_keep.h push_made).
void PushBuffer(lua_State *L, std::string &&buf) {
	thread_local std::string held;
	held = std::move(buf);
	std::string().swap(buf);
	lua_pushstring(L, held.c_str());
}
// text, changed = ui.InputTextMultiline(label, text [, capacity [, w, h [, flags]]])
int InputTextMultiline(lua_State *L) {
	const char *label = S(L, 1);
	size_t len = 0;
	const char *cur = luaL_optlstring(L, 2, "", &len);
	int cap = I(L, 3, 4096);
	cap = cap < 1 ? 1 : cap > 65535 ? 65535 : cap;
	float w = to_float(luaL_optnumber(L, 4, 0)), h = to_float(luaL_optnumber(L, 5, 0));
	ImGuiInputTextFlags flags = I(L, 6, 0) & (ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_AutoSelectAll);
	std::string buf = TextBuffer(cur, len, cap);
	bool changed = ImGui::InputTextMultiline(label, buf.data(), buf.size(), ImVec2(w, h), flags);
	PushBuffer(L, std::move(buf));
	lua_pushboolean(L, changed);
	return 2;
}

int InputText(lua_State *L) {
	const char *label = S(L, 1); // every argument checked before anything with a destructor exists
	size_t len = 0;
	const char *cur = luaL_optlstring(L, 2, "", &len);
	int cap = I(L, 3, 256);
	cap = cap < 1 ? 1 : cap > 65535 ? 65535 : cap;
	ImGuiInputTextFlags flags = I(L, 4, 0) & ~(ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_CallbackCharFilter |
		ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_CallbackCompletion);
	InputTextOptions o{L, 0, 0, false};
	if (lua_istable(L, 5)) {
		// Read raw, before the buffer exists: a metamethod lua_getfield ran could raise past its destructor.
		unview(L, 5); // (a plugin's view of the pack's options table)
		lua_pushliteral(L, "moveToEnd");
		lua_rawget(L, 5);
		o.moveToEnd = lua_toboolean(L, -1) != 0;
		lua_pop(L, 1);
		lua_pushliteral(L, "history");
		lua_rawget(L, 5);
		if (lua_isfunction(L, -1)) {
			o.history = lua_gettop(L); // left on the stack for the callback
			flags |= ImGuiInputTextFlags_CallbackHistory;
		}
		else {
			lua_pop(L, 1);
		}
		lua_pushliteral(L, "tab");
		lua_rawget(L, 5);
		if (lua_isfunction(L, -1)) {
			o.tab = lua_gettop(L); // left on the stack for the callback
			flags |= ImGuiInputTextFlags_CallbackCompletion;
		}
		else {
			lua_pop(L, 1);
		}
		if (o.moveToEnd) {
			flags |= ImGuiInputTextFlags_CallbackAlways;
		}
	}
	std::string buf = TextBuffer(cur, len, cap);
	bool changed = ImGui::InputText(label, buf.data(), buf.size(), flags, &InputTextCallback, &o);
	PushBuffer(L, std::move(buf));
	lua_pushboolean(L, changed); // with InputTextFlags.EnterReturnsTrue: Enter was pressed
	return 2;
}
int SetKeyboardFocusHere(lua_State *L) {
	ImGui::SetKeyboardFocusHere(I(L, 1, 0));
	return 0;
}
// ui.SetNavCursorVisible(bool): shows (or hides) the navigation cursor; a focus set from code (SetKeyboardFocusHere)
// leaves it hidden, and the pad's first press then only shows it.
int SetNavCursorVisible(lua_State *L) {
	ImGui::SetNavCursorVisible(lua_toboolean(L, 1) != 0);
	return 0;
}
// text, changed = ui.InputTextWithHint(label, hint, text [, capacity [, flags]])   -- the hint shows while empty
int InputTextWithHint(lua_State *L) {
	const char *label = S(L, 1);
	const char *hint = S(L, 2);
	size_t len = 0;
	const char *cur = luaL_optlstring(L, 3, "", &len);
	int cap = I(L, 4, 256);
	cap = cap < 1 ? 1 : cap > 65535 ? 65535 : cap;
	ImGuiInputTextFlags flags = I(L, 5, 0) & (ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll |
		ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_ReadOnly);
	std::string buf = TextBuffer(cur, len, cap);
	bool changed = ImGui::InputTextWithHint(label, hint, buf.data(), buf.size(), flags);
	PushBuffer(L, std::move(buf));
	lua_pushboolean(L, changed);
	return 2;
}
// open = ui.BeginCombo(label, preview [, flags]) ... ui.EndCombo()
int BeginCombo(lua_State *L) { return Ret(L, RememberPopup(ImGui::BeginCombo(S(L, 1), S(L, 2), I(L, 3, 0)), PopupKind::Combo)); }
// The popup BeginPopup, BeginCombo or BeginMenu began is the current window: what EndPopup, EndCombo and EndMenu
// need (unchecked in a release build; EndMenu reads a null ParentWindow in a top-level window).
bool InPopup(ImGuiWindowFlags also = 0) {
	const ImGuiContext &g = *GImGui;
	const ImGuiWindow *w = g.CurrentWindow;
	return OwnWindow() && w && (w->Flags & ImGuiWindowFlags_Popup) && (w->Flags & also) == also && g.BeginPopupStack.Size > 0 &&
		   g.BeginPopupStack.back().Window == w;
}
int EndCombo(lua_State *) {
	if (OwnPopup(PopupKind::Combo) && InPopup() && GImGui->BeginComboDepth > (g_floors.empty() ? 0 : g_floors.back().combos)) {
		ImGui::EndCombo();
		g_popups.pop_back();
	}
	return 0;
}
int SetItemDefaultFocus(lua_State *) {
	ImGui::SetItemDefaultFocus();
	return 0;
}
int IsItemDeactivatedAfterEdit(lua_State *L) { return Ret(L, ImGui::IsItemDeactivatedAfterEdit()); }
int ProgressBar(lua_State *L) {
	ImGui::ProgressBar(F(L, 1), ImVec2(F(L, 2, -1), F(L, 3)), lua_isnoneornil(L, 4) ? nullptr : S(L, 4));
	return 0;
}
int CollapsingHeader(lua_State *L) { return Ret(L, ImGui::CollapsingHeader(S(L, 1), I(L, 2))); }
int TreeNode(lua_State *L) { return Ret(L, ImGui::TreeNode(S(L, 1))); }
int TreePop(lua_State *) {
	if (ImGui::GetCurrentWindow()->DC.TreeDepth > (FloorWindow() ? g_floors.back().stacks.SizeOfTreeStack : 0)) {
		ImGui::TreePop();
	}
	return 0;
}

// --- images ---
// A texture argument: only a light userdata the client handed out and has not freed (ui/texture_ids.h); anything
// else (an mq.TLO value, a freed icon sheet) would reach the Vulkan backend as a descriptor set, so it is none.
ImTextureID TextureArg(lua_State *L, int i) {
	void *p = lua_type(L, i) == LUA_TLIGHTUSERDATA ? lua_touserdata(L, i) : nullptr;
	return texture_id_live(p) ? reinterpret_cast<ImTextureID>(p) : ImTextureID{};
}
// ui.Image(texture, w, h [, u0, v0, u1, v1 [, r, g, b, a]])
int Image(lua_State *L) {
	ImTextureID tex = TextureArg(L, 1);
	if (!tex) {
		ImGui::Dummy(ImVec2(F(L, 2), F(L, 3)));
		return 0;
	}
	// The tint (white by default) through ImageWithBg: Dear ImGui 1.91.9's Image takes none.
	ImGui::ImageWithBg(tex, ImVec2(F(L, 2), F(L, 3)), ImVec2(F(L, 4, 0), F(L, 5, 0)), ImVec2(F(L, 6, 1), F(L, 7, 1)),
		ImVec4(0, 0, 0, 0), Color(L, 8));
	return 0;
}
// clicked = ui.ImageButton(id, texture, w, h [, u0, v0, u1, v1])
int ImageButton(lua_State *L) {
	ImTextureID tex = TextureArg(L, 2);
	ImVec2 size(F(L, 3), F(L, 4));
	if (!tex) {
		return Ret(L, ImGui::Button(S(L, 1), size));
	}
	return Ret(L, ImGui::ImageButton(S(L, 1), tex, size, ImVec2(F(L, 5, 0), F(L, 6, 0)), ImVec2(F(L, 7, 1), F(L, 8, 1))));
}

// --- layout ---
int SameLine(lua_State *L) { ImGui::SameLine(F(L, 1), F(L, 2, -1)); return 0; }
int NewLine(lua_State *) { ImGui::NewLine(); return 0; }
int Separator(lua_State *) { ImGui::Separator(); return 0; }
int Spacing(lua_State *) { ImGui::Spacing(); return 0; }
int Dummy(lua_State *L) { ImGui::Dummy(ImVec2(F(L, 1), F(L, 2))); return 0; }
int Indent(lua_State *L) { ImGui::Indent(F(L, 1)); return 0; }
int Unindent(lua_State *L) { ImGui::Unindent(F(L, 1)); return 0; }
int BeginGroup(lua_State *) { ImGui::BeginGroup(); return 0; }
// The bindings refuse calls ImGui would only assert on (IM_ASSERT logs and carries on in this build): unbalanced
// pops and ends, and out-of-range indices, which would otherwise write outside ImGui's arrays.
int EndGroup(lua_State *) {
	if (GImGui->GroupStack.Size > (g_floors.empty() ? 0 : g_floors.back().stacks.SizeOfGroupStack)) {
		ImGui::EndGroup();
	}
	return 0;
}
int PushID(lua_State *L) {
	if (lua_type(L, 1) == LUA_TNUMBER) {
		ImGui::PushID(I(L, 1));
	}
	else {
		ImGui::PushID(S(L, 1));
	}
	return 0;
}
int PopID(lua_State *) {
	if (ImGui::GetCurrentWindow()->IDStack.Size > (FloorWindow() ? g_floors.back().stacks.SizeOfIDStack : 1)) {
		ImGui::PopID();
	}
	return 0;
}
int SetNextItemWidth(lua_State *L) { ImGui::SetNextItemWidth(F(L, 1)); return 0; }
// w, h = ui.GetContentRegionAvail()
int GetContentRegionAvail(lua_State *L) {
	ImVec2 v = ImGui::GetContentRegionAvail();
	lua_pushnumber(L, v.x);
	lua_pushnumber(L, v.y);
	return 2;
}
int GetCursorPos(lua_State *L) {
	ImVec2 v = ImGui::GetCursorPos();
	lua_pushnumber(L, v.x);
	lua_pushnumber(L, v.y);
	return 2;
}
int SetCursorPos(lua_State *L) { ImGui::SetCursorPos(ImVec2(F(L, 1), F(L, 2))); return 0; }
int SetCursorScreenPos(lua_State *L) { ImGui::SetCursorScreenPos(ImVec2(F(L, 1), F(L, 2))); return 0; }
int GetScrollY(lua_State *L) { lua_pushnumber(L, ImGui::GetScrollY()); return 1; }
int SetScrollY(lua_State *L) { ImGui::SetScrollY(F(L, 1)); return 0; }
int GetWindowPos(lua_State *L) {
	ImVec2 v = ImGui::GetWindowPos();
	lua_pushnumber(L, v.x);
	lua_pushnumber(L, v.y);
	return 2;
}
int GetWindowSize(lua_State *L) {
	ImVec2 v = ImGui::GetWindowSize();
	lua_pushnumber(L, v.x);
	lua_pushnumber(L, v.y);
	return 2;
}
// x, y, w, h = ui.FindWindowRect(name)   -- a window's place and size now, nothing for one never drawn. Called
// before the window's Begin it gives where ImGui's own title bar drag put it this frame (lib/snap.lua snaps that).
int FindWindowRect(lua_State *L) {
	const ImGuiWindow *w = ImGui::FindWindowByName(S(L, 1));
	if (!w) return 0;
	if (guarded(w) && !pack_code(L)) return 0; // the consent question's: the pack's alone
	if (w->RootWindow && w->RootWindow != w && guarded(w->RootWindow) && !pack_code(L)) return 0;
	lua_pushnumber(L, w->Pos.x);
	lua_pushnumber(L, w->Pos.y);
	lua_pushnumber(L, w->Size.x);
	lua_pushnumber(L, w->Size.y);
	return 4;
}
// ui.PushClipRect(x0, y0, x1, y1 [, intersect]) ... ui.PopClipRect()   -- drawing limited to that rectangle (screen
// pixels), not intersected with the current one unless asked: a window can draw its own frame at its very edge
// Scripts may pop only what they pushed: their pushes are counted per draw list and frame.
int PushClipRect(lua_State *L) {
	ImDrawList *dl = ImGui::GetWindowDrawList();
	if (dl != g_clipList || ImGui::GetFrameCount() != g_clipFrame) {
		g_clipList = dl;
		g_clipFrame = ImGui::GetFrameCount();
		g_clipDepth = 0;
	}
	dl->PushClipRect(ImVec2(F(L, 1), F(L, 2)), ImVec2(F(L, 3), F(L, 4)), lua_toboolean(L, 5) != 0);
	++g_clipDepth;
	return 0;
}
int PopClipRect(lua_State *) {
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const int floor = !g_floors.empty() && dl == g_floors.back().clipList ? g_floors.back().clips : 1;
	if (dl == g_clipList && ImGui::GetFrameCount() == g_clipFrame && g_clipDepth > 0 && dl->_ClipRectStack.Size > floor) {
		dl->PopClipRect();
		--g_clipDepth;
	}
	return 0;
}
int GetMousePos(lua_State *L) {
	ImVec2 m = ImGui::GetIO().MousePos;
	lua_pushnumber(L, m.x);
	lua_pushnumber(L, m.y);
	return 2;
}
int GetScrollMaxY(lua_State *L) { lua_pushnumber(L, ImGui::GetScrollMaxY()); return 1; }
int SetScrollHereY(lua_State *L) { ImGui::SetScrollHereY(F(L, 1, 0.5f)); return 0; }
int IsWindowFocused(lua_State *L) { return Ret(L, ImGui::IsWindowFocused(I(L, 1, 0))); }
// The gamepad windows' navigation (docs/gamepad.md section 5; a client extension).
int IsWindowAppearing(lua_State *L) { // false outside a window (ImGui would read a null window)
	return Ret(L, ImGui::GetCurrentContext()->CurrentWindow && ImGui::IsWindowAppearing());
}
// ui.SetWindowFocus(name): that window focused and on top; nil takes the focus from every window.
int SetWindowFocus(lua_State *L) {
	ImGui::SetWindowFocus(lua_isnoneornil(L, 1) ? nullptr : S(L, 1));
	return 0;
}

// --- tables ---
int BeginTable(lua_State *L) {
	int columns = I(L, 2, 1);
	if (columns < 1 || columns > 64) {
		return Ret(L, false);
	}
	return Ret(L, ImGui::BeginTable(S(L, 1), columns, I(L, 3), ImVec2(F(L, 4), F(L, 5))));
}
int EndTable(lua_State *) {
	if (GImGui->CurrentTable && GImGui->TablesTempDataStacked > (g_floors.empty() ? 0 : g_floors.back().tables)) ImGui::EndTable();
	return 0;
}
// The calls below refuse what ImGui would dereference or only assert on: TableNextRow (and so the header row) with
// no current table reads through a null table, and a column or freeze set up after the first row asserts.
bool TableLayoutOpen() { return GImGui->CurrentTable && !GImGui->CurrentTable->IsLayoutLocked; }
int TableNextRow(lua_State *) {
	if (GImGui->CurrentTable) {
		ImGui::TableNextRow();
	}
	return 0;
}
int TableNextColumn(lua_State *L) { return Ret(L, GImGui->CurrentTable && ImGui::TableNextColumn()); }
// BeginDisabled(disabled) / EndDisabled(): greys out and blocks the items between; ends without a begin this frame
// are ignored (ImGui asserts on them).
int g_disabledFrame = -1, g_disabledDepth = 0;
int BeginDisabled(lua_State *L) {
	if (ImGui::GetFrameCount() != g_disabledFrame) {
		g_disabledFrame = ImGui::GetFrameCount();
		g_disabledDepth = 0;
	}
	ImGui::BeginDisabled(lua_isnoneornil(L, 1) || lua_toboolean(L, 1));
	++g_disabledDepth;
	return 0;
}
int EndDisabled(lua_State *) {
	if (ImGui::GetFrameCount() == g_disabledFrame && g_disabledDepth > 0 &&
		GImGui->DisabledStackSize > (g_floors.empty() ? 0 : g_floors.back().stacks.SizeOfDisabledStack)) {
		ImGui::EndDisabled();
		--g_disabledDepth;
	}
	return 0;
}
int TableGetHoveredColumn(lua_State *L) { // 0-based column under the mouse in the current table, or -1
	lua_pushinteger(L, ImGui::GetCurrentContext()->CurrentTable ? ImGui::TableGetHoveredColumn() : -1);
	return 1;
}
int TableSetupColumn(lua_State *L) {
	const char *label = S(L, 1);
	// The status bits are ImGui's own to set (passing them asserts); ImGui clears them each frame regardless.
	const int flags = I(L, 2) & ~ImGuiTableColumnFlags_StatusMask_;
	const float width = F(L, 3);
	ImGuiTable *table = GImGui->CurrentTable;
	if (TableLayoutOpen() && table->DeclColumnsCount < table->ColumnsCount) {
		ImGui::TableSetupColumn(label, flags, width);
	}
	return 0;
}
int TableHeadersRow(lua_State *) { ImGui::TableHeadersRow(); return 0; }
// column = TableHeadersRowClicked(): the header row, as TableHeadersRow draws it; the column (0-based) clicked this
// frame, else -1 (a stock list's header click: CListWnd's notification 0xf).
int TableHeadersRowClicked(lua_State *L) {
	int clicked = -1;
	if (!GImGui->CurrentTable) {
		lua_pushinteger(L, clicked);
		return 1;
	}
	ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
	for (int c = 0, n = ImGui::TableGetColumnCount(); c < n; ++c) {
		if (!ImGui::TableSetColumnIndex(c)) continue;
		ImGui::PushID(c);
		ImGui::TableHeader(ImGui::TableGetColumnName(c));
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) clicked = c;
		ImGui::PopID();
	}
	lua_pushinteger(L, clicked);
	return 1;
}
int TableSetupScrollFreeze(lua_State *L) {
	const int columns = I(L, 1), rows = I(L, 2);
	if (TableLayoutOpen()) {
		ImGui::TableSetupScrollFreeze(std::clamp(columns, 0, IMGUI_TABLE_MAX_COLUMNS - 1), std::clamp(rows, 0, 127));
	}
	return 0;
}
// column, ascending = ui.TableGetSortSpecs()   -- after the headers of a Sortable table: the column sorted on
// (0-based) and its direction while the sort changed this frame, else nil
int TableGetSortSpecs(lua_State *L) {
	if (!GImGui->CurrentTable) {
		return 0;
	}
	ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs();
	if (!specs || !specs->SpecsDirty || specs->SpecsCount < 1) {
		return 0;
	}
	specs->SpecsDirty = false;
	lua_pushinteger(L, specs->Specs[0].ColumnIndex);
	lua_pushboolean(L, specs->Specs[0].SortDirection != ImGuiSortDirection_Descending);
	return 2;
}

// --- tabs ---
int BeginTabBar(lua_State *L) { return Ret(L, ImGui::BeginTabBar(S(L, 1), I(L, 2))); }
int EndTabBar(lua_State *) {
	if (GImGui->CurrentTabBar && GImGui->CurrentTabBarStack.Size > (g_floors.empty() ? 0 : g_floors.back().tabs)) {
		ImGui::EndTabBar();
	}
	return 0;
}
// ui.BeginTabItem(label [, flags]): flags ui.TabItemFlags (SetSelected selects it this frame, UnsavedDocument marks it)
int BeginTabItem(lua_State *L) {
    if (!g_floors.empty() && GImGui->CurrentTabBarStack.Size <= g_floors.back().tabs) return Ret(L, false);
	const bool open = ImGui::BeginTabItem(S(L, 1), nullptr, I(L, 2, 0));
	if (open) g_tabItems.push_back(GImGui->CurrentTabBar);
	return Ret(L, open);
}
int EndTabItem(lua_State *) {
	if (GImGui->CurrentTabBar && GImGui->CurrentTabBar->LastTabItemIdx >= 0 && !g_tabItems.empty() &&
		g_tabItems.back() == GImGui->CurrentTabBar && g_tabItems.size() > (g_floors.empty() ? 0 : g_floors.back().tabItems)) {
		ImGui::EndTabItem();
		g_tabItems.pop_back();
	}
	return 0;
}

// --- tooltips, popups ---
int BeginTooltip(lua_State *L) { return Ret(L, RememberPopup(ImGui::BeginTooltip(), PopupKind::Tooltip)); }
int EndTooltip(lua_State *) {
	if (OwnPopup(PopupKind::Tooltip) && GImGui->CurrentWindow && (GImGui->CurrentWindow->Flags & ImGuiWindowFlags_Tooltip)) {
		ImGui::EndTooltip();
		g_popups.pop_back();
	}
	return 0;
}
int SetTooltip(lua_State *L) { ImGui::SetTooltip("%s", S(L, 1)); return 0; }
int OpenPopup(lua_State *L) { ImGui::OpenPopup(S(L, 1)); return 0; }
int BeginPopup(lua_State *L) { return Ret(L, RememberPopup(ImGui::BeginPopup(S(L, 1)), PopupKind::Popup)); }
int BeginPopupContextItem(lua_State *L) { return Ret(L, RememberPopup(ImGui::BeginPopupContextItem(lua_isnoneornil(L, 1) ? nullptr : S(L, 1)), PopupKind::Popup)); }
// open = ui.BeginPopupContextWindow([id])   -- right-click on the window where no item (button, slot) is
int BeginPopupContextWindow(lua_State *L) {
	return Ret(L, RememberPopup(ImGui::BeginPopupContextWindow(lua_isnoneornil(L, 1) ? nullptr : S(L, 1),
		ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems), PopupKind::Popup));
}
int EndPopup(lua_State *) {
	if (OwnPopup(PopupKind::Popup) && InPopup()) {
		ImGui::EndPopup();
		g_popups.pop_back();
	}
	return 0;
}
int CloseCurrentPopup(lua_State *) {
	const int floor = g_floors.empty() ? 0 : g_floors.back().stacks.SizeOfBeginPopupStack;
	if (GImGui->BeginPopupStack.Size > floor) {
		if (floor) ImGui::ClosePopupToLevel(GImGui->BeginPopupStack.Size - 1, true);
		else ImGui::CloseCurrentPopup();
	}
	return 0;
}
// open = ui.BeginMenu(label [, enabled])   -- a submenu inside a popup or menu; ui.EndMenu() when it returned true
int BeginMenu(lua_State *L) { return Ret(L, RememberPopup(ImGui::BeginMenu(S(L, 1), B(L, 2, true)), PopupKind::Menu)); }
int EndMenu(lua_State *) {
	if (OwnPopup(PopupKind::Menu) && InPopup(ImGuiWindowFlags_ChildMenu)) {
		ImGui::EndMenu();
		g_popups.pop_back();
	}
	return 0;
}
// clicked = ui.MenuItem(label [, selected [, enabled [, shortcut]]])
int MenuItem(lua_State *L) {
	return Ret(L, ImGui::MenuItem(S(L, 1), lua_isstring(L, 4) ? lua_tostring(L, 4) : nullptr, B(L, 2), B(L, 3, true)));
}
int SetNextItemAllowOverlap(lua_State *) {
	ImGui::SetNextItemAllowOverlap();
	return 0;
}
int SetNextWindowFocus(lua_State *) {
	ImGui::SetNextWindowFocus();
	return 0;
}

// --- item state ---
int IsItemHovered(lua_State *L) { return Ret(L, ImGui::IsItemHovered(I(L, 1))); }
bool MouseButtonOk(int b) { return b >= 0 && b < ImGuiMouseButton_COUNT; }
int IsItemClicked(lua_State *L) { return Ret(L, MouseButtonOk(I(L, 1)) && ImGui::IsItemClicked(I(L, 1))); }
int IsItemActive(lua_State *L) { return Ret(L, ImGui::IsItemActive()); }
int IsItemDisabled(lua_State *L) {
	return Ret(L, (ImGui::GetCurrentContext()->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0);
}
int IsItemFocused(lua_State *L) { return Ret(L, ImGui::IsItemFocused()); } // the gamepad's navigation cursor is on it
int IsWindowHovered(lua_State *L) { return Ret(L, ImGui::IsWindowHovered(I(L, 1))); }
int IsMouseDoubleClicked(lua_State *L) { return Ret(L, MouseButtonOk(I(L, 1)) && ImGui::IsMouseDoubleClicked(I(L, 1))); }
int IsMouseClicked(lua_State *L) { return Ret(L, MouseButtonOk(I(L, 1)) && ImGui::IsMouseClicked(I(L, 1))); }
int IsMouseReleased(lua_State *L) { return Ret(L, MouseButtonOk(I(L, 1)) && ImGui::IsMouseReleased(I(L, 1))); }
int IsMouseDown(lua_State *L) { return Ret(L, MouseButtonOk(I(L, 1)) && ImGui::IsMouseDown(I(L, 1))); }
int IsMouseDragging(lua_State *L) { return Ret(L, MouseButtonOk(I(L, 1)) && ImGui::IsMouseDragging(I(L, 1))); }
int IsMouseHoveringRect(lua_State *L) {
	return Ret(L, ImGui::IsMouseHoveringRect(ImVec2(F(L, 1), F(L, 2)), ImVec2(F(L, 3), F(L, 4))));
}
int GetItemID(lua_State *L) { // the last item's ImGui id (unique per window and ID stack)
	lua_pushinteger(L, static_cast<lua_Integer>(ImGui::GetItemID()));
	return 1;
}
int GetFrameCount(lua_State *L) { // frames since ImGui started
	lua_pushinteger(L, ImGui::GetFrameCount());
	return 1;
}
int GetTime(lua_State *L) { // seconds since ImGui started
	lua_pushnumber(L, ImGui::GetTime());
	return 1;
}
int IsKeyDown(lua_State *L) {
	int key = I(L, 1);
	bool named = key >= ImGuiKey_NamedKey_BEGIN && key < ImGuiKey_NamedKey_END;
	bool modifier = key == ImGuiMod_Ctrl || key == ImGuiMod_Shift || key == ImGuiMod_Alt || key == ImGuiMod_Super;
	return Ret(L, (named || modifier) && ImGui::IsKeyDown(static_cast<ImGuiKey>(key)));
}
// ui.IsKeyPressed(key)   -- the key (ui.Key) went down this frame
int IsKeyPressed(lua_State *L) {
	int key = I(L, 1);
	bool named = key >= ImGuiKey_NamedKey_BEGIN && key < ImGuiKey_NamedKey_END;
	return Ret(L, named && ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false));
}
// The same table for every call (per Lua state), its values ImGui's for the frame, filled at the frame's first call:
// the Modern pack asks for it from every HUD window each frame (a new table each time was garbage the collector had
// to sweep). uiScale is set on each call (an option may change it within the frame). Read-only: what the caller gets
// is a view of it whose writes raise, so no window (a plugin's) can change what the windows after it read.
int ReadOnlyWrite(lua_State *L) { return luaL_error(L, "ui.GetIO(): the table is read-only"); }
int GetIO(lua_State *L) {
	static const char kView = 0, kReal = 0, kFrame = 0;
	const int frame = ImGui::GetFrameCount();
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &kReal) != LUA_TTABLE) {
		lua_pop(L, 1);
		lua_createtable(L, 0, 13);
		lua_pushvalue(L, -1);
		lua_rawsetp(L, LUA_REGISTRYINDEX, &kReal);
		lua_newtable(L); // the view
		lua_createtable(L, 0, 3);
		lua_pushvalue(L, -3);
		lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, &ReadOnlyWrite);
		lua_setfield(L, -2, "__newindex");
		lua_pushboolean(L, 0);
		lua_setfield(L, -2, "__metatable");
		lua_setmetatable(L, -2);
		lua_rawsetp(L, LUA_REGISTRYINDEX, &kView);
	}
	const int real = lua_gettop(L);
	lua_rawgetp(L, LUA_REGISTRYINDEX, &kFrame);
	const bool same_frame = lua_isinteger(L, -1) && lua_tointeger(L, -1) == frame;
	lua_pop(L, 1);
	if (!same_frame) {
		lua_pushinteger(L, frame);
		lua_rawsetp(L, LUA_REGISTRYINDEX, &kFrame);
		ImGuiIO &io = ImGui::GetIO();
		lua_pushnumber(L, io.DisplaySize.x); lua_setfield(L, real, "width");
		lua_pushnumber(L, io.DisplaySize.y); lua_setfield(L, real, "height");
		lua_pushnumber(L, io.Framerate); lua_setfield(L, real, "fps");
		lua_pushboolean(L, io.KeyCtrl); lua_setfield(L, real, "ctrl");
		lua_pushboolean(L, io.KeyShift); lua_setfield(L, real, "shift");
		lua_pushboolean(L, io.KeyAlt); lua_setfield(L, real, "alt");
		lua_pushnumber(L, io.MouseWheel); lua_setfield(L, real, "wheel");
		lua_pushnumber(L, io.MouseDelta.x); lua_setfield(L, real, "mouseDX");
		lua_pushnumber(L, io.MouseDelta.y); lua_setfield(L, real, "mouseDY");
		// The gamepad's navigation: a window takes it (NavActive) and its cursor shows (NavVisible).
		lua_pushboolean(L, io.NavActive); lua_setfield(L, real, "nav_active");
		lua_pushboolean(L, io.NavVisible); lua_setfield(L, real, "nav_visible");
		lua_pushboolean(L, io.WantTextInput); lua_setfield(L, real, "typing"); // a text field has the keyboard
	}
	lua_pushnumber(L, ui_scale()); lua_setfield(L, real, "uiScale"); // the EQ fonts' scale over their XML sizes
	lua_rawgetp(L, LUA_REGISTRYINDEX, &kView);
	return 1;
}

// --- drag and drop (string payloads) ---
int BeginDragDropSource(lua_State *L) { return Ret(L, ImGui::BeginDragDropSource(I(L, 1))); }
int SetDragDropPayload(lua_State *L) {
	size_t len = 0;
	const char *data = luaL_checklstring(L, 2, &len);
	return Ret(L, ImGui::SetDragDropPayload(S(L, 1), data, len + 1));
}
// Only after a BeginDragDropSource that returned true, in its tooltip: ImGui ends the tooltip window it began, and in
// a release build would otherwise end whatever window is current (the caller's own).
int EndDragDropSource(lua_State *) {
	const ImGuiContext &g = *GImGui;
	if (!g.DragDropActive || !g.DragDropWithinSource) return 0;
	if (!(g.DragDropSourceFlags & ImGuiDragDropFlags_SourceNoPreviewTooltip) &&
		(!g.CurrentWindow || !(g.CurrentWindow->Flags & ImGuiWindowFlags_Tooltip) || g.CurrentWindowStack.Size <= 1))
		return 0;
	ImGui::EndDragDropSource();
	return 0;
}
int BeginDragDropTarget(lua_State *L) { return Ret(L, ImGui::BeginDragDropTarget()); }
// data = ui.AcceptDragDropPayload(type) -> string or nil
int AcceptDragDropPayload(lua_State *L) {
	const ImGuiPayload *p = ImGui::AcceptDragDropPayload(S(L, 1));
	if (p && p->Data && p->DataSize > 0) {
		lua_pushlstring(L, static_cast<const char *>(p->Data), static_cast<size_t>(p->DataSize - 1)); // without its NUL
	}
	else {
		lua_pushnil(L);
	}
	return 1;
}
int EndDragDropTarget(lua_State *) {
	if (GImGui->DragDropActive && GImGui->DragDropWithinTarget) ImGui::EndDragDropTarget();
	return 0;
}

// --- style ---
int PushStyleColor(lua_State *L) {
	int idx = I(L, 1);
	if (idx >= 0 && idx < ImGuiCol_COUNT) {
		ImGui::PushStyleColor(idx, Color(L, 2));
	}
	return 0;
}
int PopStyleColor(lua_State *L) {
	int n = I(L, 1, 1);
	n = std::min(n, PoppableColors());
	if (n > 0) {
		ImGui::PopStyleColor(n);
	}
	return 0;
}
int PushStyleVar(lua_State *L) {
	int idx = I(L, 1);
	if (idx < 0 || idx >= ImGuiStyleVar_COUNT) {
		return 0;
	}
	if (lua_isnoneornil(L, 3)) {
		ImGui::PushStyleVar(I(L, 1), F(L, 2));
	}
	else {
		ImGui::PushStyleVar(I(L, 1), ImVec2(F(L, 2), F(L, 3)));
	}
	return 0;
}
// The value at list[i] as a number, or def when it is not one.
float ListNumber(lua_State *L, int list, lua_Integer i, float def) {
	lua_rawgeti(L, list, i);
	const float v = lua_isnumber(L, -1) ? to_float(lua_tonumber(L, -1)) : def;
	lua_pop(L, 1);
	return v;
}
// list[i] as an index below `count`, or -1: checked as a number before it is made an int (NaN or a huge value
// would be undefined behaviour to convert).
int ListIndex(lua_State *L, int list, lua_Integer i, int count) {
	lua_rawgeti(L, list, i);
	const double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1.0;
	lua_pop(L, 1);
	return v >= 0.0 && v < count ? static_cast<int>(v) : -1;
}
// n = ui.PushStyleColors(list)   -- many PushStyleColor in one call (lib/style.lua pushes its whole look for every
// window): list = { idx, r, g, b, a, idx, r, g, b, a, ... }. An index out of range is skipped; returns how many were
// pushed (for PopStyleColor(n)).
int PushStyleColors(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	unview(L, 1); // (a plugin's view of the pack's style lists)
	const lua_Integer len = static_cast<lua_Integer>(lua_rawlen(L, 1));
	int pushed = 0;
	for (lua_Integer i = 1; i + 4 <= len; i += 5) {
		const int idx = ListIndex(L, 1, i, ImGuiCol_COUNT);
		if (idx < 0) continue;
		ImGui::PushStyleColor(idx, ImVec4(ListNumber(L, 1, i + 1, 1), ListNumber(L, 1, i + 2, 1), ListNumber(L, 1, i + 3, 1),
		                                  ListNumber(L, 1, i + 4, 1)));
		++pushed;
	}
	lua_pushinteger(L, pushed);
	return 1;
}
// n = ui.PushStyleVars(list)   -- many PushStyleVar in one call: list = { idx, x, y, idx, x, y, ... }, y false (or
// any non-number) for a single-value variable. An index out of range is skipped; returns how many were pushed.
int PushStyleVars(lua_State *L) {
	luaL_checktype(L, 1, LUA_TTABLE);
	unview(L, 1); // (a plugin's view of the pack's style lists)
	const lua_Integer len = static_cast<lua_Integer>(lua_rawlen(L, 1));
	int pushed = 0;
	for (lua_Integer i = 1; i + 2 <= len; i += 3) {
		const int idx = ListIndex(L, 1, i, ImGuiStyleVar_COUNT);
		if (idx < 0) continue;
		const float x = ListNumber(L, 1, i + 1, 0);
		lua_rawgeti(L, 1, i + 2);
		const bool pair = lua_isnumber(L, -1) != 0;
		const float y = pair ? to_float(lua_tonumber(L, -1)) : 0.0f;
		lua_pop(L, 1);
		if (pair) {
			ImGui::PushStyleVar(idx, ImVec2(x, y));
		}
		else {
			ImGui::PushStyleVar(idx, x);
		}
		++pushed;
	}
	lua_pushinteger(L, pushed);
	return 1;
}
int PopStyleVar(lua_State *L) {
	int n = I(L, 1, 1);
	n = std::min(n, PoppableVars());
	if (n > 0) {
		ImGui::PopStyleVar(n);
	}
	return 0;
}

// --- drawing on the window (borders, overlays on icons) ---
// ui.GetItemRect([controlOnly]): total last-item bounds, or its native interaction frame when true.
int GetItemRect(lua_State *L) {
	const bool control_only = B(L, 1);
	ImVec2 a = control_only ? ImGui::GetCurrentContext()->LastItemData.NavRect.Min : ImGui::GetItemRectMin();
	ImVec2 b = control_only ? ImGui::GetCurrentContext()->LastItemData.NavRect.Max : ImGui::GetItemRectMax();
	lua_pushnumber(L, a.x); lua_pushnumber(L, a.y); lua_pushnumber(L, b.x); lua_pushnumber(L, b.y);
	return 4;
}
int GetStyleAlpha(lua_State *L) { lua_pushnumber(L, ImGui::GetStyle().Alpha); return 1; }
// ui.DrawRect(x0, y0, x1, y1, r, g, b, a [, rounding [, thickness]])  ui.DrawRectFilled(... same, no thickness)
// ui.Foreground(on)   -- the Draw* functions draw above every window (ImGui's foreground list) until turned off;
// reset each frame
int g_foregroundFrame = -1;
ImDrawList *DrawTarget() {
	return g_foregroundFrame == ImGui::GetFrameCount() ? ImGui::GetForegroundDrawList() : ImGui::GetWindowDrawList();
}
int Foreground(lua_State *L) {
	// Over every window, the consent question's too: while it is up, the pack's own code alone.
	g_foregroundFrame = lua_toboolean(L, 1) && (!g_guard || pack_code(L)) ? ImGui::GetFrameCount() : -1;
	return 0;
}
int DrawRect(lua_State *L) {
	DrawTarget()->AddRect(ImVec2(F(L, 1), F(L, 2)), ImVec2(F(L, 3), F(L, 4)),
		ImGui::GetColorU32(Color(L, 5)), F(L, 9), 0, F(L, 10, 1));
	return 0;
}
int DrawRectFilled(lua_State *L) {
	DrawTarget()->AddRectFilled(ImVec2(F(L, 1), F(L, 2)), ImVec2(F(L, 3), F(L, 4)),
		ImGui::GetColorU32(Color(L, 5)), F(L, 9));
	return 0;
}
// ui.DrawRectGradient(x0, y0, x1, y1, r, g, b, a, r2, g2, b2, a2)   -- top colour to bottom colour
int DrawRectGradient(lua_State *L) {
	ImU32 top = ImGui::GetColorU32(Color(L, 5)), bottom = ImGui::GetColorU32(Color(L, 9));
	DrawTarget()->AddRectFilledMultiColor(ImVec2(F(L, 1), F(L, 2)), ImVec2(F(L, 3), F(L, 4)), top, top, bottom, bottom);
	return 0;
}
// ui.DrawLine(x0, y0, x1, y1, r, g, b, a [, thickness])
int DrawLine(lua_State *L) {
	DrawTarget()->AddLine(ImVec2(F(L, 1), F(L, 2)), ImVec2(F(L, 3), F(L, 4)), ImGui::GetColorU32(Color(L, 5)), F(L, 9, 1));
	return 0;
}
// ui.DrawCircleFilled(x, y, radius, r, g, b, a) / ui.DrawCircle(x, y, radius, r, g, b, a [, thickness])
int DrawCircleFilled(lua_State *L) {
	DrawTarget()->AddCircleFilled(ImVec2(F(L, 1), F(L, 2)), F(L, 3), ImGui::GetColorU32(Color(L, 4)));
	return 0;
}
int DrawCircle(lua_State *L) {
	DrawTarget()->AddCircle(ImVec2(F(L, 1), F(L, 2)), F(L, 3), ImGui::GetColorU32(Color(L, 4)), 0, F(L, 8, 1));
	return 0;
}
// ui.DrawTriangleFilled(x1, y1, x2, y2, x3, y3, r, g, b, a)
int DrawTriangleFilled(lua_State *L) {
	DrawTarget()->AddTriangleFilled(ImVec2(F(L, 1), F(L, 2)), ImVec2(F(L, 3), F(L, 4)), ImVec2(F(L, 5), F(L, 6)),
		ImGui::GetColorU32(Color(L, 7)));
	return 0;
}
// ui.DrawText(x, y, r, g, b, a, text [, size])   -- size: the font's pixel height (default the current font's)
int DrawText(lua_State *L) {
	if (lua_isnoneornil(L, 8))
		DrawTarget()->AddText(ImVec2(F(L, 1), F(L, 2)), ImGui::GetColorU32(Color(L, 3)), S(L, 7));
	else
		DrawTarget()->AddText(ImGui::GetFont(), F(L, 8), ImVec2(F(L, 1), F(L, 2)), ImGui::GetColorU32(Color(L, 3)), S(L, 7));
	return 0;
}
// ui.CaptureKeyboard()   -- the keyboard is the UI's next frame, not the game's (a window that takes key presses)
int CaptureKeyboard(lua_State *) {
	ImGui::SetNextFrameWantCaptureKeyboard(true);
	return 0;
}
// ui.DrawImage(texture, x0, y0, x1, y1 [, u0, v0, u1, v1 [, r, g, b, a]])   -- screen coordinates
int DrawImage(lua_State *L) {
	ImTextureID tex = TextureArg(L, 1);
	if (tex) {
		DrawTarget()->AddImage(tex, ImVec2(F(L, 2), F(L, 3)), ImVec2(F(L, 4), F(L, 5)),
			ImVec2(F(L, 6, 0), F(L, 7, 0)), ImVec2(F(L, 8, 1), F(L, 9, 1)), ImGui::GetColorU32(Color(L, 10)));
	}
	return 0;
}
// x, y = ui.GetCursorScreenPos()
int GetCursorScreenPos(lua_State *L) {
	ImVec2 v = ImGui::GetCursorScreenPos();
	lua_pushnumber(L, v.x);
	lua_pushnumber(L, v.y);
	return 2;
}
// ui.PushFont(fontIndex) / ui.PopFont()   -- EQ UI font 1..6; an unknown number keeps the current font
int PushFont(lua_State *L) {
	ImGui::PushFont(ui_font(static_cast<int>(I(L, 1, 3))));
	return 0;
}
// ui.PushChatFont(index)   -- a chat window's font 0..10 (/chatfontsize; the original's font list); pop with PopFont
int PushChatFont(lua_State *L) {
	ImGui::PushFont(chat_font(static_cast<int>(I(L, 1, 2))));
	return 0;
}
// px = ui.ChatFontSize(index)   -- that font's cell height in pixels at UI scale 1 (0 outside 0..10)
int ChatFontSize(lua_State *L) {
	lua_pushnumber(L, chat_font_size(static_cast<int>(I(L, 1, 2))));
	return 1;
}
int PopFont(lua_State *) {
	if (PoppableFonts() > 0) {
		ImGui::PopFont();
	}
	return 0;
}

void SetInts(lua_State *L, const char *table, std::initializer_list<std::pair<const char *, int>> values) {
	lua_createtable(L, 0, static_cast<int>(values.size()));
	for (auto &v : values) {
		lua_pushinteger(L, v.second);
		lua_setfield(L, -2, v.first);
	}
	lua_setfield(L, -2, table);
}

} // namespace

std::size_t windows_begun(bool preserve_parent) {
	Floor floor;
	if (GImGui && GImGui->WithinFrameScope && GImGui->CurrentWindow) {
		floor.begun = begun().size();
		ImGui::ErrorRecoveryStoreState(&floor.stacks);
		floor.window = GImGui->CurrentWindow;
		floor.tables = GImGui->TablesTempDataStacked;
		floor.tabs = GImGui->CurrentTabBarStack.Size;
		floor.tabItems = g_tabItems.size();
		floor.popupRecords = g_popups.size();
		floor.combos = GImGui->BeginComboDepth;
		if (floor.window) {
			floor.wraps = floor.window->DC.TextWrapPosStack.Size;
			floor.clipList = floor.window->DrawList;
			floor.clips = floor.clipList->_ClipRectStack.Size;
		}
		floor.trackedClipList = g_clipList; floor.trackedClipFrame = g_clipFrame; floor.trackedClipDepth = g_clipDepth;
		floor.foregroundFrame = g_foregroundFrame;
		floor.fontScale = floor.window->FontWindowScale;
		floor.preserveParent = preserve_parent;
		if (preserve_parent) {
			floor.nextWindow = GImGui->NextWindowData; floor.nextItem = GImGui->NextItemData; floor.lastItem = GImGui->LastItemData;
			GImGui->NextWindowData.ClearFlags(); GImGui->NextItemData.ClearFlags();
		}
	}
	g_floors.push_back(floor);
	return floor.begun;
}
void end_script_windows(std::size_t depth) {
	if (!GImGui || !GImGui->WithinFrameScope) { if (!g_floors.empty()) g_floors.pop_back(); return; }
	std::vector<Begun> &b = begun();
	for (std::size_t i = depth; i < b.size(); ++i)
		if (b[i].window && b[i].clips > 0) {
			int target = b[i].clips - 1;
			if (!g_floors.empty() && b[i].window == g_floors.back().window) target = std::max(target, g_floors.back().clips);
			while (b[i].window->DrawList->_ClipRectStack.Size > target) b[i].window->DrawList->PopClipRect();
		}
	if (!g_floors.empty()) {
		const Floor& floor = g_floors.back();
		if (floor.clipList) while (floor.clipList->_ClipRectStack.Size > floor.clips) floor.clipList->PopClipRect();
		if (floor.window) while (floor.window->DC.TextWrapPosStack.Size > floor.wraps) ImGui::PopTextWrapPos();
		if (g_tabItems.size() > floor.tabItems) g_tabItems.resize(floor.tabItems);
		if (g_popups.size() > floor.popupRecords) g_popups.resize(floor.popupRecords);
		if (GImGui->BeginComboDepth > floor.combos) GImGui->BeginComboDepth = floor.combos;
		g_clipList = floor.trackedClipList; g_clipFrame = floor.trackedClipFrame; g_clipDepth = floor.trackedClipDepth;
		g_foregroundFrame = floor.foregroundFrame;
		if (floor.preserveParent && floor.window == GImGui->CurrentWindow) {
			const bool written = floor.window->WriteAccessed;
			ImGui::SetWindowFontScale(floor.fontScale);
			floor.window->WriteAccessed = written;
		}
		if (floor.preserveParent) {
			GImGui->NextWindowData = floor.nextWindow; GImGui->NextItemData = floor.nextItem; GImGui->LastItemData = floor.lastItem;
		}
		const bool preserve = floor.preserveParent;
		g_floors.pop_back();
		if (preserve) { if (b.size() > depth) b.resize(depth); return; }
	}
	if (b.size() > depth) b.resize(depth);
	GImGui->NextWindowData.ClearFlags();
	GImGui->NextItemData.ClearFlags();
}

void recover_script_windows() {
	if (!GImGui || !GImGui->WithinFrameScope || g_floors.empty()) return;
	const Floor& floor = g_floors.back();
	while (GImGui->CurrentWindowStack.Size > floor.stacks.SizeOfWindowStack) {
		ImGuiWindow* window = GImGui->CurrentWindow;
		if (g_popups.size() > floor.popupRecords && g_popups.back().window == window) {
			switch (g_popups.back().kind) {
			case PopupKind::Combo: ImGui::EndCombo(); break;
			case PopupKind::Menu: ImGui::EndMenu(); break;
			case PopupKind::Tooltip: ImGui::EndTooltip(); break;
			case PopupKind::Popup: ImGui::EndPopup(); break;
			}
			g_popups.pop_back();
		} else if (GImGui->CurrentTable && GImGui->TablesTempDataStacked > floor.tables && GImGui->CurrentTable->InnerWindow == window)
			ImGui::EndTable();
		else if (window->Flags & ImGuiWindowFlags_ChildWindow) ImGui::EndChild();
		else ImGui::End();
	}
	while (GImGui->CurrentTable && GImGui->TablesTempDataStacked > floor.tables) ImGui::EndTable();
	while (g_tabItems.size() > floor.tabItems && GImGui->CurrentTabBar && g_tabItems.back() == GImGui->CurrentTabBar) {
		ImGui::EndTabItem(); g_tabItems.pop_back();
	}
	while (GImGui->CurrentTabBar && GImGui->CurrentTabBarStack.Size > floor.tabs) ImGui::EndTabBar();
	// ImGui's generic recovery closes all current tables/tab bars. Hide only the
	// caller's preserved pointers while it recovers the callback's other stacks.
	auto* table = GImGui->CurrentTable;
	auto* tab = GImGui->CurrentTabBar;
	GImGui->CurrentTable = nullptr;
	GImGui->CurrentTabBar = nullptr;
	ImGui::ErrorRecoveryTryToRecoverState(&floor.stacks);
	GImGui->CurrentTable = table;
	GImGui->CurrentTabBar = tab;
}

void set_consent_guard(bool on) { g_guard = on; }
bool consent_guard() { return g_guard; }
void guard_current_window() { guard(ImGui::GetCurrentWindow()); }
void guard_drawing_window() {
	g_guardLuaWindow = std::string(LuaUi::drawing_window());
	g_guardLuaFrame = ImGui::GetFrameCount();
}
void raise_guarded_windows() {
	if (!g_guard) return;
	guarded_frame();
	for (ImGuiID id : g_guarded)
		if (ImGuiWindow *w = ImGui::FindWindowByID(id); w && w->Active) ImGui::BringWindowToDisplayFront(w);
}
bool pack_code(lua_State *L) {
	if (!LuaUi::caller_tag().empty()) return false;
	const bool pack = lua_getfield(L, LUA_REGISTRYINDEX, kPackStateKey) == LUA_TBOOLEAN && lua_toboolean(L, -1);
	lua_pop(L, 1);
	return pack;
}

bool ContextOnly(std::string_view name) {
	// These operations use context state, not a live window or its draw list. They remain
	// valid between frames; scoped stack changes and current-window queries do not.
	for (const char* allowed : {"GetIO", "GetTime", "GetFrameCount", "GetStyleAlpha", "GetMousePos",
		"IsMouseDoubleClicked", "IsMouseClicked", "IsMouseReleased", "IsMouseDown", "IsMouseDragging",
		"IsKeyDown", "IsKeyPressed", "GetItemID", "GetItemRect", "IsItemActive", "IsItemDisabled",
		"IsItemFocused", "IsItemDeactivatedAfterEdit", "IsWindowAppearing", "FindWindowRect", "SetWindowFocus",
		"SetNextWindowSize", "SetNextWindowSizeConstraints", "SetNextWindowPos", "SetNextWindowBgAlpha",
		"SetNextWindowCollapsed", "SetNextWindowFocus",
		"SetNextItemWidth", "SetNextItemAllowOverlap", "CaptureKeyboard", "SetNavCursorVisible",
		"SetClipboardText", "GetClipboardText"})
		if (name == allowed) return true;
	return false;
}

void register_ui_api(lua_State *L) {
	static const luaL_Reg fns[] = {
		{"Begin", Begin}, {"End", End}, {"BeginChild", BeginChild}, {"EndChild", EndChild},
		{"SetNextWindowSize", SetNextWindowSize}, {"SetNextWindowPos", SetNextWindowPos},
		{"SetNextWindowSizeConstraints", SetNextWindowSizeConstraints},
		{"SetNextWindowBgAlpha", SetNextWindowBgAlpha}, {"SetNextWindowCollapsed", SetNextWindowCollapsed}, {"IsWindowCollapsed", IsWindowCollapsed}, {"SetWindowFontScale", SetWindowFontScale},
		{"Text", Text}, {"TextColored", TextColored}, {"TextDisabled", TextDisabled}, {"TextWrapped", TextWrapped},
		{"SeparatorText", SeparatorText}, {"LabelText", LabelText}, {"BulletText", BulletText},
		{"CalcTextSize", CalcTextSize},
		{"Button", Button}, {"SmallButton", SmallButton}, {"InvisibleButton", InvisibleButton}, {"EdgeButton", EdgeButton},
		{"Selectable", Selectable}, {"Checkbox", Checkbox}, {"SliderFloat", SliderFloat}, {"SliderInt", SliderInt},
		{"ColorButton", ColorButton}, {"ColorPicker3", ColorPicker3}, {"ColorEdit3", ColorEdit3},
		{"InputText", InputText}, {"InputTextWithHint", InputTextWithHint}, {"InputTextMultiline", InputTextMultiline}, {"CodeEditor", CodeEditor}, {"SetClipboardText", SetClipboardText}, {"GetClipboardText", GetClipboardText}, {"SetKeyboardFocusHere", SetKeyboardFocusHere}, {"SetNavCursorVisible", SetNavCursorVisible}, {"BeginCombo", BeginCombo}, {"EndCombo", EndCombo},
		{"SetItemDefaultFocus", SetItemDefaultFocus}, {"IsItemDeactivatedAfterEdit", IsItemDeactivatedAfterEdit}, {"ProgressBar", ProgressBar}, {"CollapsingHeader", CollapsingHeader},
		{"TreeNode", TreeNode}, {"TreePop", TreePop},
		{"Image", Image}, {"ImageButton", ImageButton},
		{"SameLine", SameLine}, {"PushTextWrapPos", PushTextWrapPos}, {"PopTextWrapPos", PopTextWrapPos}, {"NewLine", NewLine}, {"Separator", Separator}, {"Spacing", Spacing},
		{"Dummy", Dummy}, {"Indent", Indent}, {"Unindent", Unindent}, {"BeginGroup", BeginGroup},
		{"EndGroup", EndGroup}, {"PushID", PushID}, {"PopID", PopID}, {"SetNextItemWidth", SetNextItemWidth},
		{"GetContentRegionAvail", GetContentRegionAvail}, {"GetCursorPos", GetCursorPos}, {"SetCursorPos", SetCursorPos}, {"SetCursorScreenPos", SetCursorScreenPos}, {"GetScrollY", GetScrollY}, {"SetScrollY", SetScrollY}, {"GetScrollMaxY", GetScrollMaxY}, {"GetMousePos", GetMousePos}, {"GetWindowPos", GetWindowPos}, {"Foreground", Foreground},
		{"GetWindowSize", GetWindowSize}, {"FindWindowRect", FindWindowRect}, {"PushClipRect", PushClipRect}, {"PopClipRect", PopClipRect},
		{"SetScrollHereY", SetScrollHereY}, {"IsWindowFocused", IsWindowFocused},
		{"IsWindowAppearing", IsWindowAppearing}, {"SetWindowFocus", SetWindowFocus}, {"IsItemFocused", IsItemFocused},
		{"BeginTable", BeginTable}, {"EndTable", EndTable}, {"TableNextRow", TableNextRow},
		{"TableNextColumn", TableNextColumn}, {"TableGetHoveredColumn", TableGetHoveredColumn}, {"BeginDisabled", BeginDisabled}, {"EndDisabled", EndDisabled}, {"TableSetupColumn", TableSetupColumn}, {"TableHeadersRow", TableHeadersRow}, {"TableHeadersRowClicked", TableHeadersRowClicked},
		{"TableSetupScrollFreeze", TableSetupScrollFreeze}, {"TableGetSortSpecs", TableGetSortSpecs},
		{"BeginTabBar", BeginTabBar}, {"EndTabBar", EndTabBar}, {"BeginTabItem", BeginTabItem}, {"EndTabItem", EndTabItem},
		{"BeginTooltip", BeginTooltip}, {"EndTooltip", EndTooltip}, {"SetTooltip", SetTooltip},
		{"OpenPopup", OpenPopup}, {"BeginPopup", BeginPopup}, {"BeginPopupContextItem", BeginPopupContextItem},
		{"BeginPopupContextWindow", BeginPopupContextWindow}, {"EndPopup", EndPopup}, {"BeginMenu", BeginMenu}, {"EndMenu", EndMenu}, {"CloseCurrentPopup", CloseCurrentPopup}, {"MenuItem", MenuItem}, {"SetNextWindowFocus", SetNextWindowFocus}, {"SetNextItemAllowOverlap", SetNextItemAllowOverlap},
		{"IsItemHovered", IsItemHovered}, {"IsItemClicked", IsItemClicked}, {"IsItemActive", IsItemActive}, {"IsItemDisabled", IsItemDisabled}, {"IsWindowHovered", IsWindowHovered},
		{"IsMouseDoubleClicked", IsMouseDoubleClicked}, {"IsMouseClicked", IsMouseClicked}, {"IsMouseReleased", IsMouseReleased}, {"IsMouseDown", IsMouseDown}, {"IsMouseDragging", IsMouseDragging}, {"IsMouseHoveringRect", IsMouseHoveringRect}, {"GetTime", GetTime}, {"GetFrameCount", GetFrameCount}, {"GetItemID", GetItemID}, {"IsKeyDown", IsKeyDown}, {"IsKeyPressed", IsKeyPressed}, {"CaptureKeyboard", CaptureKeyboard}, {"GetIO", GetIO},
		{"BeginDragDropSource", BeginDragDropSource}, {"SetDragDropPayload", SetDragDropPayload},
		{"EndDragDropSource", EndDragDropSource}, {"BeginDragDropTarget", BeginDragDropTarget},
		{"AcceptDragDropPayload", AcceptDragDropPayload}, {"EndDragDropTarget", EndDragDropTarget},
		{"PushStyleColor", PushStyleColor}, {"PopStyleColor", PopStyleColor}, {"PushStyleVar", PushStyleVar},
		{"PushStyleColors", PushStyleColors}, {"PushStyleVars", PushStyleVars},
		{"PopStyleVar", PopStyleVar}, {"GetItemRect", GetItemRect}, {"GetStyleAlpha", GetStyleAlpha}, {"DrawRect", DrawRect}, {"DrawRectGradient", DrawRectGradient},
		{"DrawLine", DrawLine}, {"DrawCircleFilled", DrawCircleFilled}, {"DrawCircle", DrawCircle},
		{"DrawTriangleFilled", DrawTriangleFilled},
		{"DrawRectFilled", DrawRectFilled}, {"DrawText", DrawText}, {"DrawImage", DrawImage}, {"GetCursorScreenPos", GetCursorScreenPos},
		{"PushFont", PushFont}, {"PopFont", PopFont}, {"PushChatFont", PushChatFont}, {"ChatFontSize", ChatFontSize},
		{nullptr, nullptr}};
	luaL_newlib(L, fns);
	// Retained callbacks may run while files load, before NewFrame. Context-only
	// operations remain available; widget/drawing calls need a live window.
	for (const luaL_Reg* entry = fns; entry->name; ++entry) {
		const std::string_view name(entry->name);
		if (name == "ChatFontSize" || name == "ColorButton" || name == "ColorEdit3" || name == "ColorPicker3") continue;
		const int requirement = name == "CalcTextSize" ? 1 : ContextOnly(name) ? 0 : 2;
		lua_pushcfunction(L, entry->func);
		lua_pushstring(L, entry->name);
		lua_pushinteger(L, requirement);
		lua_pushcclosure(L, [](lua_State* state) -> int {
			if (!GImGui) return luaL_error(state, "ui.%s requires an ImGui context", lua_tostring(state, lua_upvalueindex(2)));
			const auto need = lua_tointeger(state, lua_upvalueindex(3));
			if (need == 1 && !GImGui->Font)
				return luaL_error(state, "ui.%s requires an initialized ImGui font", lua_tostring(state, lua_upvalueindex(2)));
			if (need == 2 && (!GImGui->WithinFrameScope || !GImGui->CurrentWindow))
				return luaL_error(state, "ui.%s requires an active ImGui frame/window", lua_tostring(state, lua_upvalueindex(2)));
			int result = 0;
			core::cxx_guard(state, [&] { result = lua_tocfunction(state, lua_upvalueindex(1))(state); });
			return result;
		}, 3);
		lua_setfield(L, -2, entry->name);
	}
	SetInts(L, "WindowFlags", {{"None", ImGuiWindowFlags_None}, {"NoTitleBar", ImGuiWindowFlags_NoTitleBar},
		{"NoResize", ImGuiWindowFlags_NoResize}, {"NoMove", ImGuiWindowFlags_NoMove},
		{"NoScrollbar", ImGuiWindowFlags_NoScrollbar}, {"NoScrollWithMouse", ImGuiWindowFlags_NoScrollWithMouse}, {"NoCollapse", ImGuiWindowFlags_NoCollapse},
		{"AlwaysAutoResize", ImGuiWindowFlags_AlwaysAutoResize}, {"NoBackground", ImGuiWindowFlags_NoBackground},
		{"NoSavedSettings", ImGuiWindowFlags_NoSavedSettings}, {"NoMouseInputs", ImGuiWindowFlags_NoMouseInputs},
		{"MenuBar", ImGuiWindowFlags_MenuBar},
		{"HorizontalScrollbar", ImGuiWindowFlags_HorizontalScrollbar},
		{"AlwaysVerticalScrollbar", ImGuiWindowFlags_AlwaysVerticalScrollbar},
		{"AlwaysHorizontalScrollbar", ImGuiWindowFlags_AlwaysHorizontalScrollbar},
		{"NoNavInputs", ImGuiWindowFlags_NoNavInputs}, {"NoNavFocus", ImGuiWindowFlags_NoNavFocus},
		{"UnsavedDocument", ImGuiWindowFlags_UnsavedDocument}, {"NoNav", ImGuiWindowFlags_NoNav},
		{"NoFocusOnAppearing", ImGuiWindowFlags_NoFocusOnAppearing}, {"NoDecoration", ImGuiWindowFlags_NoDecoration},
		{"NoInputs", ImGuiWindowFlags_NoInputs}, {"NoBringToFrontOnFocus", ImGuiWindowFlags_NoBringToFrontOnFocus}});
	SetInts(L, "Cond", {{"Always", ImGuiCond_Always}, {"Once", ImGuiCond_Once}, {"FirstUseEver", ImGuiCond_FirstUseEver},
		{"Appearing", ImGuiCond_Appearing}});
	SetInts(L, "TableFlags", {{"None", ImGuiTableFlags_None}, {"Borders", ImGuiTableFlags_Borders},
		{"RowBg", ImGuiTableFlags_RowBg}, {"Resizable", ImGuiTableFlags_Resizable}, {"SizingFixedFit", ImGuiTableFlags_SizingFixedFit},
		{"ScrollY", ImGuiTableFlags_ScrollY}, {"PadOuterX", ImGuiTableFlags_PadOuterX},
		{"Sortable", ImGuiTableFlags_Sortable}, {"BordersInnerV", ImGuiTableFlags_BordersInnerV},
		{"BordersInnerH", ImGuiTableFlags_BordersInnerH}, {"SizingStretchProp", ImGuiTableFlags_SizingStretchProp},
		{"Hideable", ImGuiTableFlags_Hideable}, {"Reorderable", ImGuiTableFlags_Reorderable}});
	SetInts(L, "Col", {{"Text", ImGuiCol_Text}, {"TextDisabled", ImGuiCol_TextDisabled}, {"WindowBg", ImGuiCol_WindowBg},
		{"ChildBg", ImGuiCol_ChildBg}, {"Border", ImGuiCol_Border}, {"FrameBg", ImGuiCol_FrameBg}, {"FrameBgHovered", ImGuiCol_FrameBgHovered},
		{"FrameBgActive", ImGuiCol_FrameBgActive}, {"PopupBg", ImGuiCol_PopupBg}, {"Button", ImGuiCol_Button},
		{"ButtonHovered", ImGuiCol_ButtonHovered}, {"ButtonActive", ImGuiCol_ButtonActive}, {"Header", ImGuiCol_Header},
		{"PlotHistogram", ImGuiCol_PlotHistogram},
		// the rest a UI needs to restyle whole windows (ui/modern/lib/style.lua)
		{"TitleBg", ImGuiCol_TitleBg}, {"TitleBgActive", ImGuiCol_TitleBgActive}, {"TitleBgCollapsed", ImGuiCol_TitleBgCollapsed},
		{"MenuBarBg", ImGuiCol_MenuBarBg}, {"BorderShadow", ImGuiCol_BorderShadow},
		{"ScrollbarBg", ImGuiCol_ScrollbarBg}, {"ScrollbarGrab", ImGuiCol_ScrollbarGrab},
		{"ScrollbarGrabHovered", ImGuiCol_ScrollbarGrabHovered}, {"ScrollbarGrabActive", ImGuiCol_ScrollbarGrabActive},
		{"CheckMark", ImGuiCol_CheckMark}, {"SliderGrab", ImGuiCol_SliderGrab}, {"SliderGrabActive", ImGuiCol_SliderGrabActive},
		{"HeaderHovered", ImGuiCol_HeaderHovered}, {"HeaderActive", ImGuiCol_HeaderActive},
		{"Separator", ImGuiCol_Separator}, {"SeparatorHovered", ImGuiCol_SeparatorHovered}, {"SeparatorActive", ImGuiCol_SeparatorActive},
		{"ResizeGrip", ImGuiCol_ResizeGrip}, {"ResizeGripHovered", ImGuiCol_ResizeGripHovered}, {"ResizeGripActive", ImGuiCol_ResizeGripActive},
		{"Tab", ImGuiCol_Tab}, {"TabHovered", ImGuiCol_TabHovered}, {"TabSelected", ImGuiCol_TabSelected},
		{"TabSelectedOverline", ImGuiCol_TabSelectedOverline}, {"TabDimmed", ImGuiCol_TabDimmed},
		{"TabDimmedSelected", ImGuiCol_TabDimmedSelected},
		{"PlotHistogramHovered", ImGuiCol_PlotHistogramHovered},
		{"TableHeaderBg", ImGuiCol_TableHeaderBg}, {"TableBorderStrong", ImGuiCol_TableBorderStrong},
		{"TableBorderLight", ImGuiCol_TableBorderLight}, {"TableRowBg", ImGuiCol_TableRowBg}, {"TableRowBgAlt", ImGuiCol_TableRowBgAlt},
		{"TextSelectedBg", ImGuiCol_TextSelectedBg}, {"NavCursor", ImGuiCol_NavCursor},
		{"ModalWindowDimBg", ImGuiCol_ModalWindowDimBg}});
	SetInts(L, "StyleVar", {{"Alpha", ImGuiStyleVar_Alpha}, {"DisabledAlpha", ImGuiStyleVar_DisabledAlpha},
		{"WindowPadding", ImGuiStyleVar_WindowPadding},
		{"FramePadding", ImGuiStyleVar_FramePadding}, {"ItemSpacing", ImGuiStyleVar_ItemSpacing},
		{"FrameRounding", ImGuiStyleVar_FrameRounding}, {"WindowBorderSize", ImGuiStyleVar_WindowBorderSize},
		{"WindowRounding", ImGuiStyleVar_WindowRounding}, {"WindowMinSize", ImGuiStyleVar_WindowMinSize},
		{"WindowTitleAlign", ImGuiStyleVar_WindowTitleAlign}, {"ChildRounding", ImGuiStyleVar_ChildRounding},
		{"ChildBorderSize", ImGuiStyleVar_ChildBorderSize}, {"PopupRounding", ImGuiStyleVar_PopupRounding},
		{"PopupBorderSize", ImGuiStyleVar_PopupBorderSize}, {"FrameBorderSize", ImGuiStyleVar_FrameBorderSize},
		{"ItemInnerSpacing", ImGuiStyleVar_ItemInnerSpacing}, {"CellPadding", ImGuiStyleVar_CellPadding},
		{"ScrollbarSize", ImGuiStyleVar_ScrollbarSize}, {"ScrollbarRounding", ImGuiStyleVar_ScrollbarRounding},
		{"GrabMinSize", ImGuiStyleVar_GrabMinSize}, {"GrabRounding", ImGuiStyleVar_GrabRounding},
		{"TabRounding", ImGuiStyleVar_TabRounding}, {"TabBarBorderSize", ImGuiStyleVar_TabBarBorderSize},
		{"SeparatorTextBorderSize", ImGuiStyleVar_SeparatorTextBorderSize}});
	SetInts(L, "TableColumnFlags", {{"None", ImGuiTableColumnFlags_None}, {"WidthFixed", ImGuiTableColumnFlags_WidthFixed},
		{"WidthStretch", ImGuiTableColumnFlags_WidthStretch}, {"NoSort", ImGuiTableColumnFlags_NoSort},
		{"DefaultSort", ImGuiTableColumnFlags_DefaultSort}, {"PreferSortDescending", ImGuiTableColumnFlags_PreferSortDescending}});
	SetInts(L, "InputTextFlags", {{"None", 0}, {"EnterReturnsTrue", ImGuiInputTextFlags_EnterReturnsTrue},
		{"AutoSelectAll", ImGuiInputTextFlags_AutoSelectAll}, {"Password", ImGuiInputTextFlags_Password},
		{"CharsDecimal", ImGuiInputTextFlags_CharsDecimal}, {"ReadOnly", ImGuiInputTextFlags_ReadOnly}});
	SetInts(L, "ComboFlags", {{"None", 0}, {"NoArrowButton", ImGuiComboFlags_NoArrowButton},
		{"HeightLarge", ImGuiComboFlags_HeightLarge}});
	SetInts(L, "ColorEditFlags", {{"None", 0}, {"NoAlpha", ImGuiColorEditFlags_NoAlpha},
		{"NoPicker", ImGuiColorEditFlags_NoPicker}, {"NoOptions", ImGuiColorEditFlags_NoOptions},
		{"NoSmallPreview", ImGuiColorEditFlags_NoSmallPreview}, {"NoInputs", ImGuiColorEditFlags_NoInputs},
		{"NoTooltip", ImGuiColorEditFlags_NoTooltip}, {"NoLabel", ImGuiColorEditFlags_NoLabel},
		{"NoSidePreview", ImGuiColorEditFlags_NoSidePreview}, {"NoDragDrop", ImGuiColorEditFlags_NoDragDrop},
		{"NoBorder", ImGuiColorEditFlags_NoBorder}, {"AlphaOpaque", ImGuiColorEditFlags_AlphaOpaque},
		{"AlphaNoBg", ImGuiColorEditFlags_AlphaNoBg}, {"AlphaPreviewHalf", ImGuiColorEditFlags_AlphaPreviewHalf},
		{"DisplayRGB", ImGuiColorEditFlags_DisplayRGB}, {"DisplayHSV", ImGuiColorEditFlags_DisplayHSV},
		{"DisplayHex", ImGuiColorEditFlags_DisplayHex}, {"Uint8", ImGuiColorEditFlags_Uint8},
		{"Float", ImGuiColorEditFlags_Float}, {"PickerHueBar", ImGuiColorEditFlags_PickerHueBar},
		{"PickerHueWheel", ImGuiColorEditFlags_PickerHueWheel}, {"InputRGB", ImGuiColorEditFlags_InputRGB}});
	SetInts(L, "SelectableFlags", {{"None", 0}, {"SpanAllColumns", ImGuiSelectableFlags_SpanAllColumns},
		{"AllowDoubleClick", ImGuiSelectableFlags_AllowDoubleClick}});
	SetInts(L, "Mouse", {{"Left", 0}, {"Right", 1}, {"Middle", 2}});
	SetInts(L, "ButtonFlags", {{"None", 0}, {"EnableNav", ImGuiButtonFlags_EnableNav},
	                            {"MouseButtonLeft", ImGuiButtonFlags_MouseButtonLeft},
	                            {"MouseButtonRight", ImGuiButtonFlags_MouseButtonRight}});
	SetInts(L, "Key", {{"Escape", ImGuiKey_Escape}, {"Enter", ImGuiKey_Enter}, {"UpArrow", ImGuiKey_UpArrow},
	                    {"DownArrow", ImGuiKey_DownArrow}, {"LeftArrow", ImGuiKey_LeftArrow}, {"RightArrow", ImGuiKey_RightArrow},
	                    {"Space", ImGuiKey_Space}});
	// ui.Key.A .. ui.Key.Z and ui.Key["0"] .. ui.Key["9"]
	lua_getfield(L, -1, "Key");
	for (int i = 0; i < 26; ++i) {
		const char name[2] = {static_cast<char>('A' + i), 0};
		lua_pushinteger(L, ImGuiKey_A + i);
		lua_setfield(L, -2, name);
	}
	for (int i = 0; i < 10; ++i) {
		const char name[2] = {static_cast<char>('0' + i), 0};
		lua_pushinteger(L, ImGuiKey_0 + i);
		lua_setfield(L, -2, name);
	}
	lua_pop(L, 1);
	SetInts(L, "TabItemFlags", {{"None", 0}, {"UnsavedDocument", ImGuiTabItemFlags_UnsavedDocument}, {"SetSelected", ImGuiTabItemFlags_SetSelected}, {"NoTooltip", ImGuiTabItemFlags_NoTooltip}});
	SetInts(L, "TabBarFlags", {{"None", 0}, {"FittingPolicyScroll", ImGuiTabBarFlags_FittingPolicyScroll}, {"NoTooltip", ImGuiTabBarFlags_NoTooltip}});
	SetInts(L, "HoveredFlags", {{"None", 0}, {"DelayShort", ImGuiHoveredFlags_DelayShort}, {"Stationary", ImGuiHoveredFlags_Stationary}, {"ChildWindows", ImGuiHoveredFlags_ChildWindows}, {"AllowWhenBlockedByActiveItem", ImGuiHoveredFlags_AllowWhenBlockedByActiveItem},
		{"AllowWhenDisabled", ImGuiHoveredFlags_AllowWhenDisabled}});
	lua_setglobal(L, "ui");
}

} // namespace hailstone::ui::lua
