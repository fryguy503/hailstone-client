// The `ui` Lua table: Dear ImGui for scripts. Functions keep ImGui's names; values that ImGui changes through
// pointers are returned instead (value, changed = ui.Checkbox("x", value)). Colours are r, g, b, a in 0..1.
// Textures (for example item icons from eq.ItemIcon) are light userdata.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h" // stack sizes, to refuse pops and ends that ImGui would only assert on
#include "ui/imgui_layer.h"
#include "ui/lua/lua_host.h"
#include "ui/texture_ids.h"

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
std::string g_guardLuaWindow; // the pack's Lua window that asked eq.PluginConsent this frame
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
// visible, open = ui.Begin(title [, open [, flags [, collapseRight]]])   open: nil = no close button;
// collapseRight: the title bar's collapse (roll-up) button at its right, beside the close button (ImGui's
// WindowMenuButtonPosition, for this window alone), not at its left
int Begin(lua_State *L) {
	bool hasOpen = !lua_isnoneornil(L, 2);
	bool open = B(L, 2, true);
	ImGuiStyle &style = ImGui::GetStyle();
	const ImGuiDir menuButton = style.WindowMenuButtonPosition;
	if (lua_toboolean(L, 4)) style.WindowMenuButtonPosition = ImGuiDir_Right;
	bool visible = ImGui::Begin(S(L, 1), hasOpen ? &open : nullptr, WindowFlags(L, 3));
	style.WindowMenuButtonPosition = menuButton;
	if (g_guard && g_guardLuaFrame == ImGui::GetFrameCount() && LuaUi::drawing_window() == g_guardLuaWindow &&
		pack_code(L))
		guard(ImGui::GetCurrentWindow());
	lua_pushboolean(L, visible);
	lua_pushboolean(L, open);
	return 2;
}
int End(lua_State *) { ImGui::End(); return 0; }
int BeginChild(lua_State *L) { return Ret(L, ImGui::BeginChild(S(L, 1), ImVec2(F(L, 2), F(L, 3)), B(L, 4) ? ImGuiChildFlags_Borders : 0, WindowFlags(L, 5))); }
int EndChild(lua_State *) { ImGui::EndChild(); return 0; }
int SetNextWindowSize(lua_State *L) { ImGui::SetNextWindowSize(ImVec2(F(L, 1), F(L, 2)), I(L, 3, ImGuiCond_FirstUseEver)); return 0; }
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
	if (GImGui->CurrentWindow && GImGui->CurrentWindow->DC.TextWrapPosStack.Size > 0) {
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
int SliderFloat(lua_State *L) {
	float v = F(L, 2);
	bool changed = ImGui::SliderFloat(S(L, 1), &v, F(L, 3), F(L, 4, 1));
	lua_pushnumber(L, v);
	lua_pushboolean(L, changed);
	return 2;
}
// value, changed = ui.SliderInt(label, value, min, max [, text])   text: shown on the slider instead of the value
int SliderInt(lua_State *L) {
	int v = I(L, 2);
	std::string text;
	if (lua_isstring(L, 5)) {
		for (const char *c = lua_tostring(L, 5); *c; ++c) {
			if (*c == '%') text += '%';
			text += *c;
		}
	}
	bool changed = ImGui::SliderInt(S(L, 1), &v, I(L, 3), I(L, 4, 100), lua_isstring(L, 5) ? text.c_str() : "%d");
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
		lua_pushvalue(o->L, o->tab);
		lua_pushlstring(o->L, d->Buf, static_cast<size_t>(d->BufTextLen));
		lua_pushboolean(o->L, ImGui::GetIO().KeyShift);
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
// text, changed = ui.InputTextMultiline(label, text [, capacity [, w, h [, flags]]])
int InputTextMultiline(lua_State *L) {
	const char *label = S(L, 1);
	size_t len = 0;
	const char *cur = luaL_optlstring(L, 2, "", &len);
	int cap = I(L, 3, 4096);
	cap = cap < 1 ? 1 : cap > 65535 ? 65535 : cap;
	float w = to_float(luaL_optnumber(L, 4, 0)), h = to_float(luaL_optnumber(L, 5, 0));
	ImGuiInputTextFlags flags = I(L, 6, 0) & (ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_AutoSelectAll);
	std::string buf(cur, len);
	buf.resize(static_cast<size_t>(cap) + 1);
	bool changed = ImGui::InputTextMultiline(label, buf.data(), buf.size(), ImVec2(w, h), flags);
	lua_pushstring(L, buf.c_str());
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
	std::string buf(cur, len);
	buf.resize(static_cast<size_t>(cap) + 1);
	InputTextOptions o{L, 0, 0, false};
	if (lua_istable(L, 5)) {
		lua_getfield(L, 5, "moveToEnd");
		o.moveToEnd = lua_toboolean(L, -1) != 0;
		lua_pop(L, 1);
		lua_getfield(L, 5, "history");
		if (lua_isfunction(L, -1)) {
			o.history = lua_gettop(L); // left on the stack for the callback
			flags |= ImGuiInputTextFlags_CallbackHistory;
		}
		else {
			lua_pop(L, 1);
		}
		lua_getfield(L, 5, "tab");
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
	bool changed = ImGui::InputText(label, buf.data(), buf.size(), flags, &InputTextCallback, &o);
	lua_pushstring(L, buf.c_str());
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
	std::string buf(cur, len);
	buf.resize(static_cast<size_t>(cap) + 1);
	bool changed = ImGui::InputTextWithHint(label, hint, buf.data(), buf.size(), flags);
	lua_pushstring(L, buf.c_str());
	lua_pushboolean(L, changed);
	return 2;
}
// open = ui.BeginCombo(label, preview [, flags]) ... ui.EndCombo()
int BeginCombo(lua_State *L) { return Ret(L, ImGui::BeginCombo(S(L, 1), S(L, 2), I(L, 3, 0))); }
// The popup BeginPopup, BeginCombo or BeginMenu began is the current window: what EndPopup, EndCombo and EndMenu
// need (unchecked in a release build; EndMenu reads a null ParentWindow in a top-level window).
bool InPopup(ImGuiWindowFlags also = 0) {
	const ImGuiContext &g = *GImGui;
	const ImGuiWindow *w = g.CurrentWindow;
	return w && (w->Flags & ImGuiWindowFlags_Popup) && (w->Flags & also) == also && g.BeginPopupStack.Size > 0 &&
		   g.BeginPopupStack.back().Window == w;
}
int EndCombo(lua_State *) {
	if (InPopup() && GImGui->BeginComboDepth > 0) {
		ImGui::EndCombo();
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
	if (ImGui::GetCurrentWindow()->DC.TreeDepth > 0) {
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
	if (GImGui->GroupStack.Size > 0) {
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
	if (ImGui::GetCurrentWindow()->IDStack.Size > 1) {
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
ImDrawList *g_clipList = nullptr;
int g_clipFrame = -1, g_clipDepth = 0;
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
	if (dl == g_clipList && ImGui::GetFrameCount() == g_clipFrame && g_clipDepth > 0) {
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
int EndTable(lua_State *) { ImGui::EndTable(); return 0; }
// The calls below refuse what ImGui would dereference or only assert on: TableNextRow (and so the header row) with
// no current table reads through a null table, and a column or freeze set up after the first row asserts.
bool TableLayoutOpen() { return GImGui->CurrentTable && !GImGui->CurrentTable->IsLayoutLocked; }
int TableNextRow(lua_State *) {
	if (GImGui->CurrentTable) {
		ImGui::TableNextRow();
	}
	return 0;
}
int TableNextColumn(lua_State *L) { return Ret(L, ImGui::TableNextColumn()); }
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
	if (ImGui::GetFrameCount() == g_disabledFrame && g_disabledDepth > 0) {
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
	if (GImGui->CurrentTabBar) {
		ImGui::EndTabBar();
	}
	return 0;
}
// ui.BeginTabItem(label [, flags]): flags ui.TabItemFlags (SetSelected selects it this frame, UnsavedDocument marks it)
int BeginTabItem(lua_State *L) { return Ret(L, ImGui::BeginTabItem(S(L, 1), nullptr, I(L, 2, 0))); }
int EndTabItem(lua_State *) {
	if (GImGui->CurrentTabBar && GImGui->CurrentTabBar->LastTabItemIdx >= 0) {
		ImGui::EndTabItem();
	}
	return 0;
}

// --- tooltips, popups ---
int BeginTooltip(lua_State *L) { return Ret(L, ImGui::BeginTooltip()); }
int EndTooltip(lua_State *) {
	if (GImGui->CurrentWindow && (GImGui->CurrentWindow->Flags & ImGuiWindowFlags_Tooltip)) {
		ImGui::EndTooltip();
	}
	return 0;
}
int SetTooltip(lua_State *L) { ImGui::SetTooltip("%s", S(L, 1)); return 0; }
int OpenPopup(lua_State *L) { ImGui::OpenPopup(S(L, 1)); return 0; }
int BeginPopup(lua_State *L) { return Ret(L, ImGui::BeginPopup(S(L, 1))); }
int BeginPopupContextItem(lua_State *L) { return Ret(L, ImGui::BeginPopupContextItem(lua_isnoneornil(L, 1) ? nullptr : S(L, 1))); }
// open = ui.BeginPopupContextWindow([id])   -- right-click on the window where no item (button, slot) is
int BeginPopupContextWindow(lua_State *L) {
	return Ret(L, ImGui::BeginPopupContextWindow(lua_isnoneornil(L, 1) ? nullptr : S(L, 1),
		ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems));
}
int EndPopup(lua_State *) {
	if (InPopup()) {
		ImGui::EndPopup();
	}
	return 0;
}
int CloseCurrentPopup(lua_State *) { ImGui::CloseCurrentPopup(); return 0; }
// open = ui.BeginMenu(label [, enabled])   -- a submenu inside a popup or menu; ui.EndMenu() when it returned true
int BeginMenu(lua_State *L) { return Ret(L, ImGui::BeginMenu(S(L, 1), B(L, 2, true))); }
int EndMenu(lua_State *) {
	if (InPopup(ImGuiWindowFlags_ChildMenu)) {
		ImGui::EndMenu();
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
// The same table for every call in a frame (per Lua state): its values are ImGui's for the frame, set as the frame
// begins, and the Modern pack asks for it from every HUD window each frame (a new table each time was garbage the
// collector had to sweep). uiScale is set on each call (an option may change it within the frame). Read-only.
int GetIO(lua_State *L) {
	static const char kTable = 0, kFrame = 0;
	const int frame = ImGui::GetFrameCount();
	lua_rawgetp(L, LUA_REGISTRYINDEX, &kFrame);
	const bool same_frame = lua_isinteger(L, -1) && lua_tointeger(L, -1) == frame;
	lua_pop(L, 1);
	if (same_frame) {
		lua_rawgetp(L, LUA_REGISTRYINDEX, &kTable);
		if (lua_istable(L, -1)) {
			lua_pushnumber(L, ui_scale()); lua_setfield(L, -2, "uiScale");
			return 1;
		}
		lua_pop(L, 1);
	}
	ImGuiIO &io = ImGui::GetIO();
	lua_createtable(L, 0, 12);
	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &kTable);
	lua_pushinteger(L, frame);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &kFrame);
	lua_pushnumber(L, io.DisplaySize.x); lua_setfield(L, -2, "width");
	lua_pushnumber(L, io.DisplaySize.y); lua_setfield(L, -2, "height");
	lua_pushnumber(L, io.Framerate); lua_setfield(L, -2, "fps");
	lua_pushboolean(L, io.KeyCtrl); lua_setfield(L, -2, "ctrl");
	lua_pushboolean(L, io.KeyShift); lua_setfield(L, -2, "shift");
	lua_pushboolean(L, io.KeyAlt); lua_setfield(L, -2, "alt");
	lua_pushnumber(L, io.MouseWheel); lua_setfield(L, -2, "wheel");
	lua_pushnumber(L, io.MouseDelta.x); lua_setfield(L, -2, "mouseDX");
	lua_pushnumber(L, io.MouseDelta.y); lua_setfield(L, -2, "mouseDY");
	lua_pushnumber(L, ui_scale()); lua_setfield(L, -2, "uiScale"); // the EQ fonts' scale over their XML sizes
	// The gamepad's navigation: a window takes it (NavActive) and its cursor shows (NavVisible).
	lua_pushboolean(L, io.NavActive); lua_setfield(L, -2, "nav_active");
	lua_pushboolean(L, io.NavVisible); lua_setfield(L, -2, "nav_visible");
	lua_pushboolean(L, io.WantTextInput); lua_setfield(L, -2, "typing"); // a text field has the keyboard
	return 1;
}

// --- drag and drop (string payloads) ---
int BeginDragDropSource(lua_State *L) { return Ret(L, ImGui::BeginDragDropSource(I(L, 1))); }
int SetDragDropPayload(lua_State *L) {
	size_t len = 0;
	const char *data = luaL_checklstring(L, 2, &len);
	return Ret(L, ImGui::SetDragDropPayload(S(L, 1), data, len + 1));
}
int EndDragDropSource(lua_State *) { ImGui::EndDragDropSource(); return 0; }
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
int EndDragDropTarget(lua_State *) { ImGui::EndDragDropTarget(); return 0; }

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
	n = n > GImGui->ColorStack.Size ? GImGui->ColorStack.Size : n;
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
	n = n > GImGui->StyleVarStack.Size ? GImGui->StyleVarStack.Size : n;
	if (n > 0) {
		ImGui::PopStyleVar(n);
	}
	return 0;
}

// --- drawing on the window (borders, overlays on icons) ---
int GetItemRect(lua_State *L) {
	ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
	lua_pushnumber(L, a.x); lua_pushnumber(L, a.y); lua_pushnumber(L, b.x); lua_pushnumber(L, b.y);
	return 4;
}
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
// ui.PushFont(eqFont) / ui.PopFont()   -- EQ UI font 1..6; an unknown number keeps the current font
int PushFont(lua_State *L) {
	ImGui::PushFont(eq_font(static_cast<int>(I(L, 1, 3))));
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
	if (GImGui->FontStack.Size > 0) {
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

void register_ui_api(lua_State *L) {
	static const luaL_Reg fns[] = {
		{"Begin", Begin}, {"End", End}, {"BeginChild", BeginChild}, {"EndChild", EndChild},
		{"SetNextWindowSize", SetNextWindowSize}, {"SetNextWindowPos", SetNextWindowPos},
		{"SetNextWindowBgAlpha", SetNextWindowBgAlpha}, {"SetNextWindowCollapsed", SetNextWindowCollapsed}, {"IsWindowCollapsed", IsWindowCollapsed}, {"SetWindowFontScale", SetWindowFontScale},
		{"Text", Text}, {"TextColored", TextColored}, {"TextDisabled", TextDisabled}, {"TextWrapped", TextWrapped},
		{"SeparatorText", SeparatorText}, {"LabelText", LabelText}, {"BulletText", BulletText},
		{"CalcTextSize", CalcTextSize},
		{"Button", Button}, {"SmallButton", SmallButton}, {"InvisibleButton", InvisibleButton}, {"EdgeButton", EdgeButton},
		{"Selectable", Selectable}, {"Checkbox", Checkbox}, {"SliderFloat", SliderFloat}, {"SliderInt", SliderInt},
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
		{"IsItemHovered", IsItemHovered}, {"IsItemClicked", IsItemClicked}, {"IsItemActive", IsItemActive}, {"IsWindowHovered", IsWindowHovered},
		{"IsMouseDoubleClicked", IsMouseDoubleClicked}, {"IsMouseClicked", IsMouseClicked}, {"IsMouseReleased", IsMouseReleased}, {"IsMouseDown", IsMouseDown}, {"IsMouseDragging", IsMouseDragging}, {"IsMouseHoveringRect", IsMouseHoveringRect}, {"GetTime", GetTime}, {"GetFrameCount", GetFrameCount}, {"GetItemID", GetItemID}, {"IsKeyDown", IsKeyDown}, {"IsKeyPressed", IsKeyPressed}, {"CaptureKeyboard", CaptureKeyboard}, {"GetIO", GetIO},
		{"BeginDragDropSource", BeginDragDropSource}, {"SetDragDropPayload", SetDragDropPayload},
		{"EndDragDropSource", EndDragDropSource}, {"BeginDragDropTarget", BeginDragDropTarget},
		{"AcceptDragDropPayload", AcceptDragDropPayload}, {"EndDragDropTarget", EndDragDropTarget},
		{"PushStyleColor", PushStyleColor}, {"PopStyleColor", PopStyleColor}, {"PushStyleVar", PushStyleVar},
		{"PushStyleColors", PushStyleColors}, {"PushStyleVars", PushStyleVars},
		{"PopStyleVar", PopStyleVar}, {"GetItemRect", GetItemRect}, {"DrawRect", DrawRect}, {"DrawRectGradient", DrawRectGradient},
		{"DrawLine", DrawLine}, {"DrawCircleFilled", DrawCircleFilled}, {"DrawCircle", DrawCircle},
		{"DrawTriangleFilled", DrawTriangleFilled},
		{"DrawRectFilled", DrawRectFilled}, {"DrawText", DrawText}, {"DrawImage", DrawImage}, {"GetCursorScreenPos", GetCursorScreenPos},
		{"PushFont", PushFont}, {"PopFont", PopFont}, {"PushChatFont", PushChatFont}, {"ChatFontSize", ChatFontSize},
		{nullptr, nullptr}};
	luaL_newlib(L, fns);
	SetInts(L, "WindowFlags", {{"None", ImGuiWindowFlags_None}, {"NoTitleBar", ImGuiWindowFlags_NoTitleBar},
		{"NoResize", ImGuiWindowFlags_NoResize}, {"NoMove", ImGuiWindowFlags_NoMove},
		{"NoScrollbar", ImGuiWindowFlags_NoScrollbar}, {"NoScrollWithMouse", ImGuiWindowFlags_NoScrollWithMouse}, {"NoCollapse", ImGuiWindowFlags_NoCollapse},
		{"AlwaysAutoResize", ImGuiWindowFlags_AlwaysAutoResize}, {"NoBackground", ImGuiWindowFlags_NoBackground},
		{"NoSavedSettings", ImGuiWindowFlags_NoSavedSettings}, {"MenuBar", ImGuiWindowFlags_MenuBar},
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
