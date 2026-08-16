/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2026  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "ui_theme.h"
#include "utils/i18n.h"

namespace wivrn::ui
{

static ImVec4 rgb(int r, int g, int b, float a = 1)
{
	return {r / 255.f, g / 255.f, b / 255.f, a};
}

static theme dark_default()
{
	theme t;
	t.name = "Dark";
	t.localized_name = _C("theme name", "Dark");

	t.accent = rgb(30, 115, 255);
	t.accent_hovered = rgb(64, 140, 255);
	t.accent_active = rgb(18, 95, 228);
	t.on_accent = rgb(255, 255, 255);

	t.background = rgb(10, 11, 13);
	t.card = rgb(22, 24, 29);
	t.card_hovered = rgb(28, 30, 36);
	t.control = rgb(35, 38, 46);
	t.control_hovered = rgb(44, 48, 57);
	t.control_active = rgb(52, 57, 68);

	t.text = rgb(244, 245, 248);
	t.text_muted = rgb(150, 156, 167);
	t.border = rgb(40, 43, 51);

	t.danger = rgb(239, 68, 68);
	t.danger_hovered = rgb(248, 96, 96);
	t.success = rgb(52, 199, 123);
	t.warning = rgb(249, 115, 6);

	t.dimming = rgb(128, 128, 128, 0.5f);

	t.rounding = 8;
	t.card_rounding = 14;
	t.pill_rounding = 1000; // full pill on chips and toggles
	t.border_size = 1;
	t.font_scale = 1.0; // user multiplier, 100% maps to metrics::font_base
	return t;
}

static theme midnight()
{
	theme t = dark_default();
	t.name = "Midnight";
	t.localized_name = _C("theme name", "Midnight");
	t.background = rgb(6, 8, 16);
	t.card = rgb(16, 20, 34);
	t.card_hovered = rgb(22, 27, 44);
	t.control = rgb(28, 34, 54);
	t.control_hovered = rgb(36, 43, 66);
	t.control_active = rgb(44, 52, 78);
	t.border = rgb(30, 37, 58);
	return t;
}

static theme slate_light()
{
	theme t = dark_default();
	t.name = "Slate";
	t.localized_name = _C("theme name", "Slate");
	t.background = rgb(28, 30, 36);
	t.card = rgb(38, 41, 49);
	t.card_hovered = rgb(46, 50, 60);
	t.control = rgb(54, 59, 70);
	t.control_hovered = rgb(64, 70, 83);
	t.control_active = rgb(74, 81, 96);
	t.text = rgb(236, 238, 242);
	t.text_muted = rgb(168, 174, 186);
	t.border = rgb(58, 63, 75);
	return t;
}

// NX design language: liquid glass on deep space (nx-hub docs/DESIGN.md).
// Violet-cast panels meant to sit translucent over the space lobby; violet
// dominates, cyan marks live status, amber attention, red danger only.
static theme nx()
{
	theme t = dark_default();
	t.name = "NX";
	t.localized_name = _C("theme name", "NX");

	t.accent = rgb(119, 0, 255);          // NX violet #7700FF
	t.accent_hovered = rgb(154, 60, 255); // violet-soft #9a3cff
	t.accent_active = rgb(102, 0, 219);

	t.background = rgb(10, 7, 20); // deep space field #0a0714
	t.card = rgb(23, 16, 40);      // panel #171028
	t.card_hovered = rgb(29, 20, 51); // panel-2 #1d1433
	t.control = rgb(38, 28, 66);   // recessed well, one step above panel-2
	t.control_hovered = rgb(48, 36, 84);
	t.control_active = rgb(59, 45, 102);

	t.text = rgb(239, 234, 255);       // #efeaff
	t.text_muted = rgb(154, 143, 192); // #9a8fc0
	t.border = rgb(42, 31, 69);        // hairline #2a1f45

	t.danger = rgb(255, 84, 112); // #ff5470
	t.danger_hovered = rgb(255, 116, 140);
	t.success = rgb(0, 229, 255); // cyan = live #00e5ff
	t.warning = rgb(255, 179, 0); // amber = attention #ffb300

	t.dimming = rgb(10, 7, 20, 0.6f);

	// The revised language is strictly angular: radii live in the 3-6 px band and
	// nothing is pill-shaped (circles are for dots and spinners only).
	t.rounding = 5;      // --pill, the 5 px chamfer on buttons and tabs
	t.card_rounding = 6; // --radius
	t.pill_rounding = 4; // chips (--radius-xs is 3) and the toggle track, squared off
	return t;
}

// True black, ideal for OLED panels
static theme oled()
{
	theme t = dark_default();
	t.name = "OLED";
	t.localized_name = _C("theme name", "OLED");
	t.background = rgb(0, 0, 0);
	t.card = rgb(6, 6, 8);
	t.card_hovered = rgb(11, 12, 14);
	t.control = rgb(22, 24, 28);
	t.control_hovered = rgb(30, 32, 38);
	t.control_active = rgb(40, 43, 50);
	t.border = rgb(34, 24, 48); // faint violet cast, to sit with the NX accent
	return t;
}

// Neutral dark gray
static theme graphite()
{
	theme t = dark_default();
	t.name = "Graphite";
	t.localized_name = _C("theme name", "Graphite");
	t.background = rgb(20, 21, 23);
	t.card = rgb(31, 33, 36);
	t.card_hovered = rgb(39, 41, 45);
	t.control = rgb(48, 51, 56);
	t.control_hovered = rgb(58, 62, 68);
	t.control_active = rgb(68, 72, 80);
	t.text = rgb(238, 240, 243);
	t.text_muted = rgb(160, 164, 172);
	t.border = rgb(50, 53, 59);
	return t;
}

// Light surfaces, dark text
static theme light()
{
	theme t = dark_default();
	t.name = "Light";
	t.localized_name = _C("theme name", "Light");
	t.background = rgb(188, 193, 202);
	t.card = rgb(252, 252, 253);
	t.card_hovered = rgb(242, 243, 246);
	t.control = rgb(206, 210, 219);
	t.control_hovered = rgb(193, 198, 208);
	t.control_active = rgb(178, 184, 196);
	t.text = rgb(14, 16, 20);
	t.text_muted = rgb(66, 72, 84);
	t.border = rgb(176, 182, 194);
	t.success = rgb(36, 147, 90);
	return t;
}

std::vector<theme> presets()
{
	return {nx(), oled(), dark_default(), midnight(), graphite(), slate_light(), light()};
}

std::vector<accent_swatch> accent_swatches()
{
	return {
	        {"NX", rgb(119, 0, 255), rgb(143, 43, 255), rgb(102, 0, 219)},
	        {"Blue", rgb(30, 115, 255), rgb(64, 140, 255), rgb(18, 95, 228)},
	        {"Violet", rgb(132, 72, 255), rgb(152, 98, 255), rgb(112, 52, 232)},
	        {"Emerald", rgb(0, 200, 132), rgb(28, 214, 150), rgb(0, 172, 112)},
	        {"Amber", rgb(255, 160, 0), rgb(255, 176, 36), rgb(228, 140, 0)},
	        {"Rose", rgb(255, 40, 82), rgb(255, 74, 108), rgb(230, 28, 68)},
	        {"Cyan", rgb(0, 190, 225), rgb(34, 205, 236), rgb(0, 168, 200)},
	};
}

void set_accent(const accent_swatch & swatch)
{
	theme & t = current();
	t.accent = swatch.base;
	t.accent_hovered = swatch.hovered;
	t.accent_active = swatch.active;
}

theme & current()
{
	static theme t = dark_default();
	return t;
}

void set_theme(const theme & t)
{
	current() = t;
}

float & background_alpha()
{
	static float alpha = 0.75f; // translucent panels by default
	return alpha;
}

void theme::apply() const
{
	ImGuiStyle & style = ImGui::GetStyle();

	style.FontScaleMain = font_scale;
	style.WindowRounding = card_rounding;
	style.ChildRounding = card_rounding;
	style.PopupRounding = card_rounding;
	style.FrameRounding = rounding;
	style.GrabRounding = rounding;
	style.TabRounding = rounding;
	style.FrameBorderSize = border_size;
	style.WindowBorderSize = border_size;

	ImVec4 * c = style.Colors;
	c[ImGuiCol_Text] = text;
	c[ImGuiCol_TextDisabled] = text_muted;
	c[ImGuiCol_WindowBg] = background;
	c[ImGuiCol_ChildBg] = card;
	c[ImGuiCol_PopupBg] = card;
	c[ImGuiCol_Border] = border;
	c[ImGuiCol_FrameBg] = control;
	c[ImGuiCol_FrameBgHovered] = control_hovered;
	c[ImGuiCol_FrameBgActive] = control_active;
	c[ImGuiCol_Button] = control;
	c[ImGuiCol_ButtonHovered] = control_hovered;
	c[ImGuiCol_ButtonActive] = accent;
	c[ImGuiCol_Header] = control_hovered;
	c[ImGuiCol_HeaderHovered] = control_hovered;
	c[ImGuiCol_HeaderActive] = accent;
	c[ImGuiCol_SliderGrab] = accent;
	c[ImGuiCol_SliderGrabActive] = accent_active;
	c[ImGuiCol_CheckMark] = accent;
	c[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
	c[ImGuiCol_ScrollbarGrab] = control;
	c[ImGuiCol_ScrollbarGrabHovered] = control_hovered;
	c[ImGuiCol_ScrollbarGrabActive] = control_active;
	c[ImGuiCol_Separator] = border;
}

} // namespace wivrn::ui
