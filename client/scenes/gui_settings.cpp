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

// each page builds a list of `setting` descriptors, render_settings() draws the matching
// ui:: widget for each, rebuilt every frame so dynamic options/descriptions just work
// same pages serve the lobby and the in-stream window, gated by ctx.in_game

#define IMGUI_DEFINE_MATH_OPERATORS
#include "scenes/gui_common.h"

#include "gui_settings.h"

#include "application.h"
#include "configuration.h"
#include "decoder/decoder.h"
#include "render/imgui_impl.h"
#include "render/ui_theme.h"
#include "render/ui_widgets.h"
#include "utils/i18n.h"
#include "xr/instance.h"
#include "xr/session.h"
#include "xr/system.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <boost/locale.hpp>
#include <cmath>
#include <functional>
#include <locale>
#include <optional>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <tuple>
#include <vector>

namespace ui = wivrn::ui;

namespace
{
constexpr float control_w = ui::metrics::setting_control_width;

float toggle_width()
{
	return ImGui::GetFrameHeight() * ui::metrics::control_height * ui::metrics::toggle_aspect;
}

enum class ui_kind
{
	toggle,
	slider,
	segmented,
	combo,
	combo_multi,
	button,
};

// one declarative setting, `enabled` greys the row out without hiding it
struct setting
{
	const char * id; // stable imgui id
	std::string label;
	std::string description;
	ui_kind ui = ui_kind::toggle;

	std::function<bool()> get_bool;
	std::function<void(bool)> set_bool;

	std::function<int()> get_int;
	std::function<void(int)> set_int;
	int v_min = 0;
	int v_max = 0;

	std::function<bool(int)> get_multi;
	std::function<void(int, bool)> set_multi;

	std::string fmt;
	std::function<std::vector<std::string>()> options;
	std::string title; // combo modal title

	std::string button_label;       // ui_kind::button
	std::function<void()> on_click; // ui_kind::button

	std::optional<bool> default_bool;               // reset target for toggles
	std::optional<int> default_int;                 // reset target for slider/segmented/combo
	std::optional<std::vector<char>> default_multi; // reset target for multicombo

	std::function<bool()> enabled;
	std::string disabled_tooltip; // shown on a disabled row
};

void render_settings(const wivrn::gui::settings_context & ctx, const char * card_id, const std::vector<setting> & list)
{
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ui::metrics::card_item_spacing);
	ui::begin_card(card_id);

	for (const auto & s: list)
	{
		const bool enabled = s.enabled ? s.enabled() : true;
		ImGui::BeginDisabled(not enabled);

		// stable storage for the reset defaults during this call
		bool def_b = s.default_bool.value_or(false);
		int def_i = s.default_int.value_or(0);
		const bool * dpb = s.default_bool ? &def_b : nullptr;
		const int * dpi = s.default_int ? &def_i : nullptr;
		const std::vector<char> * dpm = s.default_multi ? &s.default_multi.value() : nullptr;

		const ImVec2 row_min = ImGui::GetCursorScreenPos();

		const float w = s.ui == ui_kind::toggle ? toggle_width() + (s.default_bool ? ui::reset_slot_width() : 0) : control_w;
		const float label_bottom = ui::setting_label(s.label, s.description, w);

		switch (s.ui)
		{
			case ui_kind::toggle: {
				bool v = s.get_bool();
				if (ui::toggle(s.id, &v, dpb))
					s.set_bool(v);
				break;
			}
			case ui_kind::slider: {
				int v = s.get_int();
				if (ui::slider_int(s.id, &v, s.v_min, s.v_max, s.fmt.c_str(), {control_w, 0}, dpi))
					s.set_int(v);
				break;
			}
			case ui_kind::segmented: {
				const auto opts = s.options();
				int v = s.get_int();
				if (ui::segmented(s.id, opts, &v, {control_w, 0}, dpi))
					s.set_int(v);
				break;
			}
			case ui_kind::combo: {
				const auto opts = s.options();
				std::vector<ui::combo_item> items;
				for (const auto & o: opts)
					items.push_back({o.c_str()});
				int v = s.get_int();
				if (ui::combo(s.id, s.title, items, &v, control_w, dpi))
					s.set_int(v);
				break;
			}
			case ui_kind::combo_multi: {
				const auto opts = s.options();
				std::vector<ui::combo_item> items;
				std::vector<char> selected;
				for (const auto & o: opts)
					items.push_back({o.c_str()});
				for (size_t i = 0; i < items.size(); ++i)
					selected.push_back(s.get_multi(i));

				if (ui::combo_multi(s.id, s.title, s.title, items, &selected, control_w, dpm))
				{
					// TODO only set the changed value
					for (size_t i = 0; i < items.size(); ++i)
						s.set_multi(i, selected[i]);
				}
				break;
			}
			case ui_kind::button: {
				if (ui::button(s.button_label, ui::button_style::secondary, {control_w, 0}) and s.on_click)
					s.on_click();
				break;
			}
		}

		ImGui::EndDisabled();

		// reserve row height for a multi-line description plus padding, Dummy grows content size
		const float pad = std::max(label_bottom - ImGui::GetCursorPosY(), 0.f) + ui::metrics::label_bottom_pad;
		ImGui::Dummy({0, pad});

		// disabled rows can't fire the widget tooltip hook, do it on the row
		if (not enabled and not s.disabled_tooltip.empty())
		{
			const ImVec2 row_max{row_min.x + ImGui::GetContentRegionAvail().x, ImGui::GetCursorScreenPos().y};
			if (ImGui::IsMouseHoveringRect(row_min, row_max))
				ctx.imgui_ctx.tooltip(s.disabled_tooltip, (row_min + row_max) / 2);
		}
	}

	ui::end_card();
	ImGui::PopStyleVar();
}

template <typename Range, typename T>
std::optional<int> index(const Range & range, const T & value)
{
	auto it = std::ranges::find(range, value);
	if (it == range.end())
		return std::nullopt;
	else
		return (it - range.begin());
}

} // namespace

namespace wivrn::gui
{

void settings_video(const settings_context & ctx)
{
	auto & config = ctx.config;
	auto & default_config = ctx.default_config;
	const std::string disconnect_tip = ctx.in_game ? _C("tooltip for disabled settings", "Disconnect to change this setting.") : std::string{};
	std::vector<setting> list;

	if (const auto rates = ctx.session.get_refresh_rates(); not rates.empty())
	{
		int default_rate_index = index(rates, default_config.preferred_refresh_rate).value_or(-1) + 1;

		list.push_back({
		        .id = "##refresh",
		        .label = _("Refresh rate"),
		        .description = _("Use 'auto' to select the refresh rate based on measured application performance. May cause flicker when a change happens."),
		        .ui = ui_kind::segmented,
		        .get_int = [&config, rates] {
			        for (size_t i = 0; i < rates.size(); ++i)
				        if (rates[i] == config.preferred_refresh_rate)
					        return int(i) + 1;
			        return 0; },
		        .set_int = [&ctx, &config, rates](int v) {
			        if (v == 0)
			        {
				        config.preferred_refresh_rate = 0;
				        config.fps_divider = 1;
			        }
			        else
			        {
				        ctx.session.set_refresh_rate(rates[v - 1]);
				        config.preferred_refresh_rate = rates[v - 1];
			        }
			        config.save();
			        if (ctx.on_streaming_changed)
				        ctx.on_streaming_changed(); },
		        .options = [rates] {
			        std::vector<std::string> opts;
			        opts.push_back(_C("automatic refresh rate, use a short string", "Auto"));
			        for (float r: rates)
				        opts.push_back(fmt::format("{}", int(r)));
			        return opts; },
		        .default_int = default_rate_index,
		});
	}

	{
		const auto width = ctx.recommended_width;
		const auto height = ctx.recommended_height;
		list.push_back({
		        .id = "##resolution",
		        .label = _("Render resolution"),
		        .description = fmt::format(_cF("render resolution", "Pixels rendered per eye ({}x{}). Lower to gain performance."), int(width * config.resolution_scale), int(height * config.resolution_scale)),
		        .ui = ui_kind::slider,
		        .get_int = [&config] { return int(std::lround(config.resolution_scale * 100)); },
		        .set_int = [&config](int v) { config.resolution_scale = v / 100.0; config.save(); },
		        .v_min = 50,
		        .v_max = config.extended_config ? 350 : 150,
		        .fmt = "%d%%",
		        .default_int = int(std::lround(default_config.resolution_scale * 100)),
		        .enabled = [&ctx] { return not ctx.in_game; },
		        .disabled_tooltip = disconnect_tip,
		});
	}

	if (ctx.instance.has_extension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
	{
		list.push_back({
		        .id = "##spacewarp",
		        .label = _("Half framerate mode"),
		        .description = _("Stream at half the refresh rate. The image is sent, encoded and decoded half as often, which saves bandwidth and power; every frame is simply shown twice, nothing is synthesised."),
		        .ui = ui_kind::toggle,
		        .get_bool = [&config] { return config.fps_divider == 2; },
		        .set_bool = [&ctx, &config](bool v) { config.fps_divider = v ? 2 : 1; config.save(); if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
		        .default_bool = default_config.fps_divider == 2,
		        .enabled = [&config] { return config.preferred_refresh_rate != 0; },
		        .disabled_tooltip = _("Set a refresh rate other than auto to enable this setting."),
		});
	}

	ui::page_header(_cS("page header title", "Video"), _cS("page header subtitle", "Frame rate and resolution."));
	render_settings(ctx, "##video", list);
}

void settings_streaming(const settings_context & ctx)
{
	auto & config = ctx.config;
	auto & default_config = ctx.default_config;
	const std::string disconnect_tip = ctx.in_game ? _C("tooltip for disabled settings", "Disconnect to change this setting.") : std::string{};
	std::vector<setting> list;

	list.push_back({
	        .id = "##standby_freeze",
	        .label = _("Freeze sleeping controllers"),
	        .description = _("Some controllers, Pico's in particular, go to sleep on their own and keep reporting a pose the headset knows is no longer being tracked. On, the computer holds the controller at the last pose it was really tracked at until it wakes up. Off, the computer uses whatever pose the headset reports, which is what WiVRn has always done and which can make a sleeping controller jump across the room."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.standby_freeze; },
	        .set_bool = [&ctx, &config](bool v) { config.standby_freeze = v; config.save(); if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
	        .default_bool = default_config.standby_freeze,
	});

	list.push_back({
	        .id = "##foveation",
	        .label = _("Foveated encoding"),
	        .description = config.check_feature(feature::eye_gaze)
	                               ? _("Higher values focus image quality where you look at, improving latency, power efficiency and quality.")
	                               : _("Higher values focus image quality at the center, improving latency, power efficiency and quality."),
	        .ui = ui_kind::slider,
	        .get_int = [&config] { return int(std::lround((1 - config.get_stream_scale()) * 100)); },
	        .set_int = [&config](int v) {
		        if (not config.extended_config)
			        v = std::clamp(v, 30, 80);
		        config.set_stream_scale(1 - v * 0.01);
		        config.save(); },
	        .v_min = 0,
	        .v_max = 80,
	        .fmt = "%d%%",
	        .default_int = int(std::lround((1 - default_config.get_stream_scale()) * 100)),
	        .enabled = [&ctx] { return not ctx.in_game; },
	        .disabled_tooltip = disconnect_tip,
	});

	list.push_back({
	        .id = "##reduced_resolution",
	        .label = _("Reduced resolution streaming"),
	        .description = _("Encode, transmit and decode the video at a fraction of the normal resolution to save bitrate, encode and decode cost. The display resolution is unchanged; the smaller image is reconstructed on the headset when it is drawn. Pairs with FSR upscaling on the Post-processing page for sharpness (without it the reconstruction is a plain bilinear stretch). Takes effect on the next connection."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.reduced_resolution; },
	        .set_bool = [&ctx, &config](bool v) { config.reduced_resolution = v; config.save(); if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
	        .default_bool = default_config.reduced_resolution,
	});

	list.push_back({
	        .id = "##render_scale",
	        .label = _C("setting name", "Streaming resolution"),
	        .description = _("Fraction of the normal encode resolution the video is streamed at. Lower saves more bandwidth and power but softens the image; pair it with FSR upscaling. Takes effect on the next connection."),
	        .ui = ui_kind::slider,
	        .get_int = [&config] { return int(std::lround(config.render_scale * 100)); },
	        .set_int = [&ctx, &config](int v) { config.render_scale = std::clamp(v, 50, 100) * 0.01f; config.save(); if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
	        .v_min = 50,
	        .v_max = 100,
	        .fmt = "%d%%",
	        .default_int = int(std::lround(default_config.render_scale * 100)),
	        .enabled = [&config] { return config.reduced_resolution; },
	        .disabled_tooltip = _("Enable reduced resolution streaming to change this setting."),
	});

	list.push_back({
	        .id = "##sharper_center",
	        .label = _("Sharper center (foveation)"),
	        .description = _("Reshape the foveation curve so the center of the image stays full resolution over a wider area and the periphery falls off more steeply, spending more of the same encoded size on where you are looking. The encoded size, bitrate and display resolution are unchanged, so it costs nothing extra and takes effect immediately without reconnecting. Best on headsets without eye tracking, where the sharp region is fixed at the center."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.sharper_center; },
	        .set_bool = [&ctx, &config](bool v) { config.sharper_center = v; config.save(); if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
	        .default_bool = default_config.sharper_center,
	});

	list.push_back({
	        .id = "##foveation_strength",
	        .label = _C("setting name", "Sharper center strength"),
	        .description = _("How much the center is favored. Higher widens the full-resolution center and steepens the periphery falloff. The server clamps the steepest periphery when the streaming resolution is low so the edges do not collapse into a blocky upscale."),
	        .ui = ui_kind::slider,
	        .get_int = [&config] { return int(std::lround(config.foveation_strength * 100)); },
	        .set_int = [&ctx, &config](int v) { config.foveation_strength = std::clamp(v, 0, 100) * 0.01f; config.save(); if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
	        .v_min = 0,
	        .v_max = 100,
	        .fmt = "%d%%",
	        .default_int = int(std::lround(default_config.foveation_strength * 100)),
	        .enabled = [&config] { return config.sharper_center; },
	        .disabled_tooltip = _("Enable sharper center to change this setting."),
	});

	list.push_back({
	        .id = "##foveation_adaptive",
	        .label = _("Adaptive foveation"),
	        .description = _("Let the server steepen the center-sharpening curve on its own when the link degrades, so the periphery compresses under a Wi-Fi dip instead of the whole image losing quality. Only the curve shape changes, never the encoded size, so it stays live. Needs the automatic bitrate."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.foveation_adaptive; },
	        .set_bool = [&ctx, &config](bool v) { config.foveation_adaptive = v; config.save(); if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
	        .default_bool = default_config.foveation_adaptive,
	        .enabled = [&config] { return config.sharper_center and config.bitrate_auto; },
	        .disabled_tooltip = _("Enable sharper center and the automatic bitrate to use adaptive foveation."),
	});

	list.push_back({
	        .id = "##foveation_foveal_qp",
	        .label = _("Protect foveal quality (NVENC/x264)"),
	        .description = _("Bias the encoder to spend fewer bits compressing the fixed central region, where the encoder exposes a per-region quality map. Only NVENC and x264 have such a path; VAAPI and Vulkan encoders ignore it. Takes effect on the next connection."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.foveation_foveal_qp; },
	        .set_bool = [&ctx, &config](bool v) { config.foveation_foveal_qp = v; config.save(); if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
	        .default_bool = default_config.foveation_foveal_qp,
	        .enabled = [&config] { return config.sharper_center; },
	        .disabled_tooltip = _("Enable sharper center to change this setting."),
	});

	auto codec_name = [](std::optional<wivrn::video_codec> codec) -> std::string {
		if (not codec)
			return _C("Codec", "Automatic");
		switch (*codec)
		{
			case wivrn::h264:
				return _C("Codec", "H.264");
			case wivrn::h265:
				return _C("Codec", "HEVC (H.265)");
			case wivrn::av1:
				return _C("Codec", "AV1");
			case wivrn::raw:
				break;
		}
		return _C("Codec", "Automatic");
	};

	std::vector<wivrn::video_codec> codecs;
	for (auto c: wivrn::decoder::supported_codecs())
		if (c != wivrn::raw)
			codecs.push_back(c);

	list.push_back({
	        .id = "##codec",
	        .label = _C("setting name", "Video codec"),
	        .description = _("How video is compressed before it is sent to the headset."),
	        .ui = ui_kind::combo,
	        .get_int = [&config, codecs] { return config.codec ? index(codecs, *config.codec).value_or(-1) + 1 : 0; },
	        .set_int = [&config, codecs](int v) {
		        config.codec = v == 0 ? std::nullopt : std::optional(codecs[v - 1]);
		        config.save(); },
	        .options = [codecs, codec_name] {
		        std::vector<std::string> opts;
		        opts.push_back(codec_name(std::nullopt));
		        for (auto c: codecs)
			        opts.push_back(codec_name(c));
		        return opts; },
	        .title = _C("setting name", "Video codec"),
	        .default_int = default_config.codec ? index(codecs, *default_config.codec).value_or(-1) + 1 : 0,
	        .enabled = [&ctx] { return not ctx.in_game; },
	        .disabled_tooltip = disconnect_tip,
	});

	if (config.codec == wivrn::h265 or config.codec == wivrn::av1)
	{
		list.push_back({
		        .id = "##ten_bit",
		        .label = _C("setting name", "10-bit color"),
		        .description = _("Higher color precision, supported by HEVC and AV1."),
		        .ui = ui_kind::toggle,
		        .get_bool = [&config] { return config.bit_depth == 10; },
		        .set_bool = [&config](bool v) { config.bit_depth = v ? 10 : 8; config.save(); },
		        .default_bool = default_config.bit_depth == 10,
		        .enabled = [&ctx] { return not ctx.in_game; },
		        .disabled_tooltip = disconnect_tip,
		});
	}

	// Manual / Adaptive / Adaptive v2. bitrate_auto stays the switch it always was — the
	// dashboard and the server both still read it — and bitrate_bbr only says which control
	// law the adaptive entries mean. Leaving bitrate_bbr empty (nobody ever touched this)
	// hands the choice to the server's own configuration, which is why picking "Adaptive"
	// explicitly is not the same as never having picked anything.
	const auto bitrate_control_index = [](const configuration & c) {
		if (not c.bitrate_auto)
			return 0;
		return c.bitrate_bbr.value_or(false) ? 2 : 1;
	};

	list.push_back({
	        .id = "##bitrate_control",
	        .label = _C("setting name", "Bitrate control"),
	        .description = _("Manual keeps the bitrate below exactly where you set it. Adaptive lets the server lower it when the connection cannot keep up, using that bitrate as the maximum. Adaptive v2 is experimental: instead of reacting to congestion it measures how fast frames are actually being delivered and aims just under that."),
	        .ui = ui_kind::combo,
	        .get_int = [&config, bitrate_control_index] { return bitrate_control_index(config); },
	        .set_int = [&ctx, &config](int v) {
		        config.bitrate_auto = v != 0;
		        if (v != 0)
			        config.bitrate_bbr = v == 2;
		        config.save();
		        if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
	        .options = [] { return std::vector<std::string>{
		                        _C("Bitrate control", "Manual"),
		                        _C("Bitrate control", "Adaptive"),
		                        _C("Bitrate control", "Adaptive v2 (experimental)"),
		                }; },
	        .title = _C("setting name", "Bitrate control"),
	        .default_int = bitrate_control_index(default_config),
	});

	list.push_back({
	        .id = "##radio_aware",
	        .label = _("Radio-aware bitrate"),
	        .description = _("Report the Wi-Fi signal to the server about once a second. A signal that has been falling for a few seconds means you are walking away from the access point, and the server lowers the bitrate before the packets start dropping instead of after. Only the trend is used, and it can only ever lower the bitrate."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.radio_aware; },
	        .set_bool = [&ctx, &config](bool v) { config.radio_aware = v; config.save(); if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
	        .default_bool = default_config.radio_aware,
	        .enabled = [&config] { return config.bitrate_auto; },
	        .disabled_tooltip = _("Enable automatic bitrate to change this setting."),
	});

	const int mb = 1'000'000;
	list.push_back({
	        .id = "##bitrate",
	        .label = _("Bitrate"),
	        .description = _("Video data rate sent to the headset."),
	        .ui = ui_kind::slider,
	        .get_int = [&config] { return int(config.bitrate_bps / mb); },
	        .set_int = [&ctx, &config](int v) { config.bitrate_bps = uint32_t(v) * mb; config.save(); if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
	        .v_min = 5,
	        .v_max = int(config.max_bitrate() / mb),
	        .fmt = "%d Mbit/s",
	        .default_int = int(default_config.bitrate_bps / mb),
	});

	list.push_back({
	        .id = "##smooth_pacing",
	        .label = _("Smooth packet pacing"),
	        .description = _("Spread each video frame's packets evenly over part of a frame period instead of sending them in one burst. The burst is what overflows a Wi-Fi access point's buffer and causes the lag spikes it then takes seconds to recover from."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.smooth_pacing; },
	        .set_bool = [&ctx, &config](bool v) { config.smooth_pacing = v; config.save(); if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
	        .default_bool = default_config.smooth_pacing,
	});

	list.push_back({
	        .id = "##fec",
	        .label = _("Error correction (FEC)"),
	        .description = _("Send a small amount of redundant data alongside the video so that a packet the Wi-Fi drops is rebuilt here instead of losing the whole frame and waiting for a new keyframe. Costs about 12% of the bandwidth, which the server takes out of the video quality rather than adding on top, so the amount on the wire does not change."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.fec; },
	        .set_bool = [&ctx, &config](bool v) { config.fec = v; config.save(); if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
	        .default_bool = default_config.fec,
	});

	list.push_back({
	        .id = "##fec_adaptive",
	        .label = _("Adaptive error correction"),
	        .description = _("Let the computer decide how much redundant data to send from how much is actually going missing: barely any on a clean connection, a lot more when packets start dropping. It also spreads each protected group out over the stream, so that a burst of packets lost together costs one repairable hole in several groups rather than wiping out one of them. Costs nothing extra on a good connection and protects better on a bad one."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.fec_adaptive; },
	        .set_bool = [&ctx, &config](bool v) { config.fec_adaptive = v; config.save(); if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
	        .default_bool = default_config.fec_adaptive,
	        .enabled = [&config] { return config.fec; },
	        .disabled_tooltip = _("Enable error correction to change this setting."),
	});

	list.push_back({
	        .id = "##shard_retransmit",
	        .label = _("Shard retransmission"),
	        .description = _("When a video packet goes missing and the redundant data cannot rebuild it, ask the computer to send that one packet again. On a home network the answer comes back in two or three milliseconds, which is well inside the time the frame has to be shown, so the frame is finished instead of being thrown away and a fresh keyframe requested."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.shard_retransmit; },
	        .set_bool = [&ctx, &config](bool v) { config.shard_retransmit = v; config.save(); if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
	        .default_bool = default_config.shard_retransmit,
	});

	list.push_back({
	        .id = "##wifi_qos",
	        .label = _("Wi-Fi QoS priority"),
	        .description = _("Tag the streaming traffic so that the access point puts it in its high priority queues ahead of everything else on the network. A few networks mangle or drop tagged traffic instead; turn this off if the connection is worse with it on."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.wifi_qos; },
	        .set_bool = [&ctx, &config](bool v) { config.wifi_qos = v; config.save(); if (ctx.on_qos_changed) ctx.on_qos_changed(); },
	        .default_bool = default_config.wifi_qos,
	});

	list.push_back({
	        .id = "##encoder_failover",
	        .label = _("Encoder failover"),
	        .description = _("If the graphics card's video encoder stops working in the middle of a session, let the computer carry on encoding that eye in software instead of leaving it frozen until you reconnect. Costs CPU on the computer while it lasts, and only works for H.264."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.encoder_failover; },
	        .set_bool = [&ctx, &config](bool v) { config.encoder_failover = v; config.save(); if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
	        .default_bool = default_config.encoder_failover,
	});

	list.push_back({
	        .id = "##intra_refresh",
	        .label = _("Intra-refresh recovery"),
	        .description = _("When part of a frame is lost for good, let the computer repair the picture gradually over the next half second instead of resending a whole fresh image at once. The full image is the biggest thing the connection ever has to carry, and it is asked for exactly when the connection is at its worst, which is how one glitch turns into several. The gradual repair keeps the data rate steady instead. Turning it on takes effect on the next connection; not all encoders can do it."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.intra_refresh; },
	        .set_bool = [&ctx, &config](bool v) { config.intra_refresh = v; config.save(); if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
	        .default_bool = default_config.intra_refresh,
	});

	// Off / Backup only / Combine. multipath_usb stays the switch it always was —
	// off is the one state that means "never attach a path at all" — and
	// multipath_combine only says what the path is for once there is one, the same
	// shape as bitrate_auto and bitrate_bbr above.
	const auto multipath_index = [](const configuration & c) {
		if (not c.multipath_usb)
			return 0;
		return c.multipath_combine ? 2 : 1;
	};

	list.push_back({
	        .id = "##multipath_usb",
	        .label = _("USB connection"),
	        .description = _("Use the USB cable alongside Wi-Fi while streaming. Requires the tunnel to be armed by the WiVRn dashboard.\n\nBackup only: the cable stands by, and video and input switch to it automatically when the Wi-Fi link fails, then switch back once it has been stable again.\n\nCombine (experimental): both links carry video at once. Wi-Fi keeps everything it can deliver in time and the rest of each frame goes over the cable, so the two bandwidths add up. Falls back to backup behaviour the moment either link struggles."),
	        .ui = ui_kind::combo,
	        .get_int = [&config, multipath_index] { return multipath_index(config); },
	        .set_int = [&ctx, &config](int v) {
		        config.multipath_usb = v != 0;
		        if (v != 0)
			        config.multipath_combine = v == 2;
		        config.save();
		        if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
	        .options = [] { return std::vector<std::string>{
		                        _C("USB connection", "Off"),
		                        _C("USB connection", "Backup only"),
		                        _C("USB connection", "Combine (experimental)"),
		                }; },
	        .title = _("USB connection"),
	        .default_int = multipath_index(default_config),
	});

	list.push_back({
	        .id = "##sharp_text",
	        .label = _("Text clarity mode"),
	        .description = _("Optimise the encoder for fine detail such as text and user interfaces, at the expense of a slightly noisier image. Takes effect on the next connection."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.sharp_text; },
	        .set_bool = [&ctx, &config](bool v) { config.sharp_text = v; config.save(); if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
	        .default_bool = default_config.sharp_text,
	});

	list.push_back({
	        .id = "##quad_layers",
	        .label = _("Sharp overlay layers"),
	        .description = _("Stream overlay panels such as WayVR windows as their own layer instead of mixing them into the game image: they come out sharper, and stay perfectly still when you move your head. Takes effect on the next connection."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.quad_layers; },
	        .set_bool = [&ctx, &config](bool v) { config.quad_layers = v; config.save(); if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
	        .default_bool = default_config.quad_layers,
	});

	list.push_back({
	        .id = "##comfort_vignette",
	        .label = _("Comfort vignette on lag"),
	        .description = _("Darken the edges of the view when the application no longer keeps up with the display, to reduce discomfort."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.comfort_vignette; },
	        .set_bool = [&config](bool v) { config.comfort_vignette = v; config.save(); },
	        .default_bool = default_config.comfort_vignette,
	});

	// Off / Headset / Server. motion_smoothing stays the switch it always was, and
	// motion_smoothing_server only says which end does the warping when it is on —
	// the same shape as bitrate_auto and bitrate_bbr above.
	const auto motion_smoothing_index = [](const configuration & c) {
		if (not c.motion_smoothing)
			return 0;
		return c.motion_smoothing_server ? 2 : 1;
	};

	list.push_back({
	        .id = "##motion_smoothing",
	        .label = _("Motion smoothing"),
	        .description = _("When the application runs below the display rate, shift the last image along the motion the server measured between application frames instead of repeating it unchanged. Smooths out judder at the cost of some smearing around moving edges. Only active while the application is actually behind.\n\nHeadset does the shifting here, and costs no extra bandwidth. Server (experimental): the PC warps frames before encoding. Spends bitrate on synthesized frames; slightly less exact timing than headset mode."),
	        .ui = ui_kind::combo,
	        .get_int = [&config, motion_smoothing_index] { return motion_smoothing_index(config); },
	        .set_int = [&ctx, &config](int v) {
		        config.motion_smoothing = v != 0;
		        if (v != 0)
			        config.motion_smoothing_server = v == 2;
		        config.save();
		        if (ctx.on_streaming_changed) ctx.on_streaming_changed(); },
	        .options = [] { return std::vector<std::string>{
		                        _C("Motion smoothing", "Off"),
		                        _C("Motion smoothing", "Headset"),
		                        _C("Motion smoothing", "Server (experimental)"),
		                }; },
	        .title = _("Motion smoothing"),
	        .default_int = motion_smoothing_index(default_config),
	});

	// in-stream: steer where foveation focuses quality
	if (ctx.in_game)
	{
		list.push_back({
		        .id = "##fov_override",
		        .label = _C("setting name", "Foveation center override"),
		        .description = _("Manually set where image quality is focused, instead of the center or your gaze."),
		        .ui = ui_kind::toggle,
		        .get_bool = [&config] { return config.override_foveation_enable; },
		        .set_bool = [&ctx, &config](bool v) {
			        config.override_foveation_enable = v;
			        config.save();
			        if (ctx.on_foveation_override_changed)
				        ctx.on_foveation_override_changed(); },
		        .default_bool = default_config.override_foveation_enable,
		});
		list.push_back({
		        .id = "##fov_adjust",
		        .label = _C("setting name", "Foveation center"),
		        .description = fmt::format(_F("Height {:.1f} °, distance {:.2f} m"), -config.override_foveation_pitch * 180 / M_PI, config.override_foveation_distance),
		        .ui = ui_kind::button,
		        .button_label = _C("button label to change the foveation center", "Change"),
		        .on_click = [&ctx] { if (ctx.enter_foveation_adjust) ctx.enter_foveation_adjust(); },
		        .enabled = [&config] { return config.override_foveation_enable; },
		        .disabled_tooltip = _("Enable foveation center override to change this setting."),
		});
	}

	ui::page_header(_cS("page header title", "Streaming"), _cS("page header subtitle", "How video is encoded and sent to the headset."));
	render_settings(ctx, "##streaming", list);

	if (wivrn::ui::confirm_modal(
	            "confirm disable in stream gui",
	            _C("confirmation messagebox title", "Disable in-stream window"),
	            _C("confirmation message", "Do you really want to disable the in-stream window?\nYou will not be able to re-open it with the thumbsticks."),
	            _("Yes"),
	            _("No"),
	            true) == 1)
	{
		config.enable_stream_gui = false;
		config.save();
	}
}

void settings_post_processing(const settings_context & ctx)
{
	auto & config = ctx.config;
	auto & default_config = ctx.default_config;
	std::vector<setting> list;

	if (application::get_openxr_post_processing_supported())
	{
		auto flag_name = [](XrCompositionLayerSettingsFlagsFB f) -> std::string {
			switch (f)
			{
				case XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB:
				case XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB:
					return _C("openxr_post_processing", "Normal");
				case XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB:
				case XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB:
					return _C("openxr_post_processing", "Quality");
				default:
					return _C("openxr_post_processing", "Disabled");
			}
		};

		auto flag_combo = [&](const char * id, std::string label, std::string desc, std::array<XrCompositionLayerSettingsFlagsFB, 3> flags, XrCompositionLayerSettingsFlagsFB configuration::openxr_post_processing_settings::* member) {
			std::string title = label;
			list.push_back({
			        .id = id,
			        .label = std::move(label),
			        .description = std::move(desc),
			        .ui = ui_kind::combo,
			        .get_int = [&config, flags, member] { return index(flags, config.openxr_post_processing.*member).value_or(0); },
			        .set_int = [&config, flags, member](int v) {
				        config.openxr_post_processing.*member = flags[v];
				        config.save(); },
			        .options = [flags, flag_name] {
				        std::vector<std::string> o;
				        for (auto f: flags)
					        o.push_back(flag_name(f));
				        return o; },
			        .title = std::move(title),
			        .default_int = index(flags, config.openxr_post_processing.*member).value_or(0),
			});
		};

		flag_combo("##supersampling", _("Supersampling"), _("Reduce flicker for high contrast edges. Useful when the input resolution is high compared to the headset display."), {0, XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB, XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB}, &configuration::openxr_post_processing_settings::super_sampling);
		flag_combo("##sharpening", _("Sharpening"), _("Improve clarity of high contrast edges and counteract blur. Useful when the input resolution is low compared to the headset display."), {0, XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB, XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB}, &configuration::openxr_post_processing_settings::sharpening);
	}

	list.push_back({
	        .id = "##fsr",
	        .label = _("FSR upscaling"),
	        .description = _("Reconstruct the decoded video with AMD FSR 1 (edge-adaptive upscaling plus sharpening) instead of a plain bilinear stretch. Sharpest when the stream is encoded below the display resolution, so it pairs with 'Reduced resolution streaming' on the Streaming page, but it also works at full resolution as a mild sharpen and anti-alias. Costs about a dozen extra texture reads per pixel, so it is off by default and replaces contrast adaptive sharpening while it is on."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.fsr; },
	        .set_bool = [&config](bool v) { config.fsr = v; config.save(); },
	        .default_bool = default_config.fsr,
	});

	list.push_back({
	        .id = "##fsr_sharpness",
	        .label = _C("setting name", "FSR sharpness"),
	        .description = _("How strongly FSR's RCAS pass sharpens the reconstructed image. 0% leaves a pure edge-adaptive upscale."),
	        .ui = ui_kind::slider,
	        .get_int = [&config] { return int(std::lround(config.fsr_sharpness * 100)); },
	        .set_int = [&config](int v) { config.fsr_sharpness = v * 0.01f; config.save(); },
	        .v_min = 0,
	        .v_max = 100,
	        .fmt = "%d%%",
	        .default_int = int(std::lround(default_config.fsr_sharpness * 100)),
	        .enabled = [&config] { return config.fsr; },
	        .disabled_tooltip = _("Enable FSR upscaling to change this setting."),
	});

	list.push_back({
	        .id = "##cas_sharpening",
	        .label = _("Contrast adaptive sharpening"),
	        .description = _("Sharpen the decoded video, more where the image is flat and less where it already has contrast."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.cas_sharpening; },
	        .set_bool = [&config](bool v) { config.cas_sharpening = v; config.save(); },
	        .default_bool = default_config.cas_sharpening,
	        .enabled = [&config] { return not config.fsr; },
	        .disabled_tooltip = _("FSR upscaling already sharpens the image; turn it off to use contrast adaptive sharpening instead."),
	});

	list.push_back({
	        .id = "##cas_sharpness",
	        .label = _C("setting name", "Sharpening strength"),
	        .description = _("How much the contrast adaptive sharpening filter sharpens the image."),
	        .ui = ui_kind::slider,
	        .get_int = [&config] { return int(std::lround(config.cas_sharpness * 100)); },
	        .set_int = [&config](int v) { config.cas_sharpness = v * 0.01f; config.save(); },
	        .v_min = 0,
	        .v_max = 100,
	        .fmt = "%d%%",
	        .default_int = int(std::lround(default_config.cas_sharpness * 100)),
	        .enabled = [&config] { return config.cas_sharpening and not config.fsr; },
	        .disabled_tooltip = config.fsr ? _("FSR upscaling already sharpens the image; turn it off to use contrast adaptive sharpening instead.") : _("Enable contrast adaptive sharpening to change this setting."),
	});

	list.push_back({
	        .id = "##cas_full_kernel",
	        .label = _("Full sharpening kernel"),
	        .description = _("Use the full 3x3 sharpening kernel, including the diagonal samples, instead of the cheaper 5-tap cross. The cross halves the texture reads this pass makes and looks almost identical on a compressed streamed image; the full kernel is slightly crisper at diagonal edges but costs more GPU."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.cas_full_kernel; },
	        .set_bool = [&config](bool v) { config.cas_full_kernel = v; config.save(); },
	        .default_bool = default_config.cas_full_kernel,
	        .enabled = [&config] { return config.cas_sharpening and not config.fsr; },
	        .disabled_tooltip = config.fsr ? _("FSR upscaling already sharpens the image; turn it off to use contrast adaptive sharpening instead.") : _("Enable contrast adaptive sharpening to change this setting."),
	});

	list.push_back({
	        .id = "##ambient_glow",
	        .label = _("Ambient glow"),
	        .description = _("Bleed the frame's edge colours outward into the black periphery beyond the headset's field of view as a soft glow. Widens the perceived field of view, eases motion sickness and softens the cutoff when motion smoothing cannot keep up."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.ambient_glow; },
	        .set_bool = [&config](bool v) { config.ambient_glow = v; config.save(); },
	        .default_bool = default_config.ambient_glow,
	});

	list.push_back({
	        .id = "##ambient_glow_intensity",
	        .label = _C("setting name", "Ambient glow intensity"),
	        .description = _("How strongly the peripheral colour wash bleeds inward."),
	        .ui = ui_kind::slider,
	        .get_int = [&config] { return int(std::lround(config.ambient_glow_intensity * 100)); },
	        .set_int = [&config](int v) { config.ambient_glow_intensity = v * 0.01f; config.save(); },
	        .v_min = 0,
	        .v_max = 100,
	        .fmt = "%d%%",
	        .default_int = int(std::lround(default_config.ambient_glow_intensity * 100)),
	        .enabled = [&config] { return config.ambient_glow; },
	        .disabled_tooltip = _("Enable ambient glow to change this setting."),
	});

	list.push_back({
	        .id = "##deband",
	        .label = _("Debanding"),
	        .description = _("Dither the decoded video to break up the colour banding that 8-bit output and video compression leave in smooth gradients, such as skyboxes, fog and dark rooms. Especially visible on OLED. Nearly free and on by default."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.deband; },
	        .set_bool = [&config](bool v) { config.deband = v; config.save(); },
	        .default_bool = default_config.deband,
	});

	list.push_back({
	        .id = "##deband_strength",
	        .label = _C("setting name", "Debanding strength"),
	        .description = _("How strongly the debanding dither is applied, as a fraction of one 8-bit step. 100% dithers by one step, which removes most banding; raise it if banding is still visible on a dark OLED gradient."),
	        .ui = ui_kind::slider,
	        .get_int = [&config] { return int(std::lround(config.deband_strength * 100)); },
	        .set_int = [&config](int v) { config.deband_strength = v * 0.01f; config.save(); },
	        .v_min = 0,
	        .v_max = 200,
	        .fmt = "%d%%",
	        .default_int = int(std::lround(default_config.deband_strength * 100)),
	        .enabled = [&config] { return config.deband; },
	        .disabled_tooltip = _("Enable debanding to change this setting."),
	});

	list.push_back({
	        .id = "##reduce_gpu_load",
	        .label = _("Reduce GPU load (experimental)"),
	        .description = _("Skip re-rendering the streamed image on refreshes where nothing has changed, re-presenting the previous one instead. Saves GPU power when the application runs below the display rate; head tracking stays responsive because the runtime still reprojects every refresh. Watch the Defoveate meter in the Statistics tab to see the effect."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.reduce_gpu_load; },
	        .set_bool = [&config](bool v) { config.reduce_gpu_load = v; config.save(); },
	        .default_bool = default_config.reduce_gpu_load,
	});

	ui::page_header(_S("Post-processing"), _cS("page header subtitle", "OpenXR layer supersampling and sharpening."));
	render_settings(ctx, "##post_processing", list);
}

void settings_audio(const settings_context & ctx)
{
	auto & config = ctx.config;
	auto & default_config = ctx.default_config;
	const std::string disconnect_tip = ctx.in_game ? _C("tooltip for disabled settings", "Disconnect to change this setting.") : std::string{};
	std::vector<setting> list;

	list.push_back({
	        .id = "##microphone",
	        .label = _C("setting name", "Microphone"),
	        .description = _("Stream the headset microphone to the PC."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.check_feature(feature::microphone); },
	        .set_bool = [&config](bool v) { config.set_feature(feature::microphone, v); },
	        .default_bool = default_config.check_feature(feature::microphone),
	        .enabled = [&ctx] { return not ctx.in_game; },
	        .disabled_tooltip = disconnect_tip,
	});

	list.push_back({
	        .id = "##unprocessed",
	        .label = _C("setting name", "Unprocessed audio"),
	        .description = _("Disable audio filters, such as noise cancellation."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.mic_unprocessed_audio; },
	        .set_bool = [&config](bool v) { config.mic_unprocessed_audio = v; config.save(); },
	        .default_bool = default_config.mic_unprocessed_audio,
	        .enabled = [&ctx, &config] { return not ctx.in_game and config.check_feature(feature::microphone); },
	        .disabled_tooltip = disconnect_tip,
	});

	list.push_back({
	        .id = "##low_latency_audio",
	        .label = _C("setting name", "Low-latency audio path"),
	        .description = _("Send audio on the same loss-tolerant path as the video. A dropped packet is concealed instead of stalling the sound while it is resent."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.low_latency_audio; },
	        .set_bool = [&config, &ctx](bool v) {
		        config.low_latency_audio = v;
		        config.save();
		        if (ctx.on_audio_path_changed)
			        ctx.on_audio_path_changed(); },
	        .default_bool = default_config.low_latency_audio,
	});

	ui::page_header(_cS("page header title", "Audio"), _cS("page header subtitle", "Microphone streamed to the PC."));
	render_settings(ctx, "##audio", list);
}

void settings_devices(const settings_context & ctx)
{
	auto & config = application::get_config();
	auto & default_config = ctx.default_config;
	std::vector<setting> list;

	list.push_back({
	        .id = "##keyboard",
	        .label = _C("setting name", "Keyboard"),
	        .description = _("Forward the keyboard from the headset to the PC."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.forward_keyboard; },
	        .set_bool = [&config](bool v) { config.forward_keyboard = v; config.save(); },
	        .default_bool = default_config.forward_keyboard,
	});

	list.push_back({
	        .id = "##mouse",
	        .label = _C("setting name", "Mouse"),
	        .description = _("Forward the mouse from the headset to the PC."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.forward_mouse; },
	        .set_bool = [&config](bool v) { config.forward_mouse = v; config.save(); },
	        .default_bool = default_config.forward_mouse,
	});

	list.push_back({
	        .id = "##gamepad",
	        .label = _C("setting name", "Gamepad"),
	        .description = _("Forward the gamepad from the headset to the PC."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.forward_gamepad; },
	        .set_bool = [&config](bool v) { config.forward_gamepad = v; config.save(); },
	        .default_bool = default_config.forward_gamepad,
	});

	ui::page_header(_cS("page header title", "Devices"), _cS("page header subtitle", "Forward input devices to the PC."));
	render_settings(ctx, "##devices", list);

	if (ctx.server_hid_forwarding == false and (config.forward_keyboard or config.forward_mouse or config.forward_gamepad))
		ui::chip(_("The server does not allow forwarded input devices"), ui::chip_style::warning);
}

bool settings_tracking(const settings_context & ctx)
{
	auto & config = ctx.config;
	auto & default_config = ctx.default_config;
	const std::string disconnect_tip = ctx.in_game ? _C("tooltip for disabled settings", "Disconnect to change this setting.") : std::string{};
	std::vector<setting> list;

	auto feature_toggle = [&](const char * id, std::string label, std::string desc, feature f) {
		list.push_back({
		        .id = id,
		        .label = std::move(label),
		        .description = std::move(desc),
		        .ui = ui_kind::toggle,
		        .get_bool = [&config, f] { return config.check_feature(f); },
		        .set_bool = [&config, f](bool v) { config.set_feature(f, v); },
		        .default_bool = default_config.check_feature(f),
		        .enabled = [&ctx] { return not ctx.in_game; },
		        .disabled_tooltip = disconnect_tip,
		});
	};

	if (ctx.system.hand_tracking_supported())
		feature_toggle("##hand", _C("setting name", "Hand tracking"), _("Track your hands for input when controllers are down."), feature::hand_tracking);

	if (application::get_eye_gaze_supported())
		feature_toggle("##eye", _C("setting name", "Eye tracking"), _("Used by foveated encoding to focus quality where you look."), feature::eye_gaze);

	if (ctx.system.face_tracker_supported() != xr::face_tracker_type::none)
		feature_toggle("##face", _C("setting name", "Face tracking"), _("Stream facial expressions to the PC."), feature::face_tracking);

	const auto body_tracker = ctx.system.body_tracker_supported();

	std::vector<char> body_parts_default;
	std::vector<std::string> body_parts_names;
	std::vector<from_headset::body_part_mask> body_parts_bit;
	for (const auto & [bit, name]: {
	             std::make_pair(from_headset::body_part_mask::chest, _C("virtual body tracker selection", "Chest")),
	             std::make_pair(from_headset::body_part_mask::left_elbow, _C("virtual body tracker selection", "Left elbow")),
	             std::make_pair(from_headset::body_part_mask::right_elbow, _C("virtual body tracker selection", "Right elbow")),
	             std::make_pair(from_headset::body_part_mask::hip, _C("virtual body tracker selection", "Hip")),
	             std::make_pair(from_headset::body_part_mask::left_knee, _C("virtual body tracker selection", "Left knee")),
	             std::make_pair(from_headset::body_part_mask::right_knee, _C("virtual body tracker selection", "Right knee")),
	             std::make_pair(from_headset::body_part_mask::left_foot, _C("virtual body tracker selection", "Left foot")),
	             std::make_pair(from_headset::body_part_mask::right_foot, _C("virtual body tracker selection", "Right foot")),
	     })
	{
		if (body_tracker == xr::body_tracker_type::fb and bit > from_headset::body_part_mask::hip)
			break;
		body_parts_default.push_back((default_config.body_part_mask & std::to_underlying(bit)) != 0);
		body_parts_names.push_back(name);
		body_parts_bit.push_back(bit);
	}

	bool changed = false;
	if (body_tracker != xr::body_tracker_type::none)
	{
		list.push_back({
		        .id = "##body",
		        .label = _C("setting name", "Body tracking"),
		        .description = body_tracker == xr::body_tracker_type::fb or body_tracker == xr::body_tracker_type::meta
		                               ? _("Requires 'Hand and body tracking' to be enabled in the Quest movement tracking settings, otherwise body data will be guessed from controller and headset positions.")
		                               : _("Stream body joint positions to the PC."),
		        .ui = ui_kind::toggle,
		        .get_bool = [&config] { return config.check_feature(feature::body_tracking); },
		        .set_bool = [&config](bool v) { config.set_feature(feature::body_tracking, v); },
		        .default_bool = default_config.check_feature(feature::body_tracking),
		        .enabled = [&ctx] { return not ctx.in_game; },
		        .disabled_tooltip = disconnect_tip,
		});

		list.push_back({
		        .id = "##body_tracking_parts",
		        .label = _C("setting name", "Virtual body trackers"),
		        .description = _("Create virtual tracker devices."),
		        .ui = ui_kind::combo_multi,
		        .get_multi = [&config, &body_parts_bit](int index) {
			        const auto underlying = std::to_underlying(body_parts_bit[index]);
			        return config.body_part_mask & underlying; },
		        .set_multi = [&config, &body_parts_bit, &changed](int index, bool value) {
			        const auto underlying = std::to_underlying(body_parts_bit[index]);
				if (value)
					config.body_part_mask |= underlying;
				else
					config.body_part_mask &= ~underlying;
				config.save();
				changed = true; },
		        .options = [&body_parts_names]() { return body_parts_names; },
		        .title = _C("setting name", "Virtual body trackers"),
		        .default_multi = body_parts_default,
		        .disabled_tooltip = disconnect_tip,
		});
	}

	ui::page_header(_cS("page header title", "Tracking"), _cS("page header subtitle", "Body and input tracking sent to the PC."));
	render_settings(ctx, "##tracking", list);

	return changed;
}

void settings_system(const settings_context & ctx)
{
	auto & config = ctx.config;
	auto & default_config = ctx.default_config;
	std::vector<setting> list;

	auto language_name = [](const std::locale & loc = std::locale()) {
		return boost::locale::pgettext("language selection, replace with the name of the language", "English", loc);
	};
	std::vector<std::tuple<std::string, std::locale, std::string>> languages;
	for (const auto & msg_info: get_locales())
	{
		std::string code = msg_info.language;
		if (not msg_info.country.empty())
			code += "_" + msg_info.country;
		std::locale loc(std::locale(), boost::locale::gnu_gettext::create_messages_facet<char>(msg_info));
		languages.emplace_back(language_name(loc), loc, std::move(code));
	}
	std::ranges::sort(languages, [](auto & l, auto & r) { return std::get<0>(l) < std::get<0>(r); });

	auto language_index = [languages](const configuration & cfg) {
		if (cfg.locale != "")
			for (size_t i = 0; i < languages.size(); ++i)
				if (std::get<2>(languages[i]) == cfg.locale)
					return int(i) + 1;
		return 0;
	};

	list.push_back({
	        .id = "##language",
	        .label = _("Language"),
	        .description = _C("setting name", "Interface language."),
	        .ui = ui_kind::combo,
	        .get_int = [&config, language_index] { return language_index(config); },
	        .set_int = [&config, languages](int v) {
		        config.locale = v == 0 ? "" : std::get<2>(languages[v - 1]);
		        config.save();
		        application::instance().load_locale(); },
	        .options = [languages] {
		        std::vector<std::string> opts;
		        opts.push_back(_C("item in the interface language combobox", "System language"));
		        for (const auto & [lang, loc, code]: languages)
			        opts.push_back(lang);
		        return opts; },
	        .title = _("Language"),
	        .default_int = language_index(default_config),
	});

	list.push_back({
	        .id = "##extended",
	        .label = _C("setting name", "Extended configuration"),
	        .description = _("Allows unsafe configuration values, use at your own risk."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config] { return config.extended_config; },
	        .set_bool = [&config](bool v) { config.extended_config = v; config.save(); },
	        .default_bool = default_config.extended_config,
	});

	if (ctx.instance.has_extension(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME))
	{
		list.push_back({
		        .id = "##high_power",
		        .label = _("High power mode"),
		        .description = _("Increase power usage to allow higher resolution and refresh rate. Drains battery and runs hot."),
		        .ui = ui_kind::toggle,
		        .get_bool = [&config] { return config.high_power_mode; },
		        .set_bool = [&config](bool v) { config.high_power_mode = v; config.save(); },
		        .default_bool = default_config.high_power_mode,
		});
	}

	bool disable_instream_gui = false;
	bool disable_instream_gui_popup_open = ImGui::IsPopupOpen("confirm disable in stream gui");
	list.push_back({
	        .id = "##stream_gui",
	        .label = _C("setting name", "In-stream window"),
	        .description = _("Enables the configuration window to be shown while the game is streaming. If enabled, the window is activated by pressing both thumbsticks."),
	        .ui = ui_kind::toggle,
	        .get_bool = [&config, &disable_instream_gui_popup_open] { return config.enable_stream_gui and not disable_instream_gui_popup_open; },
	        .set_bool = [&ctx, &config, &disable_instream_gui](bool v) {
			if (ctx.in_game and not v)
			{
				disable_instream_gui = true;
			}
			else
			{
				config.enable_stream_gui = v;
				config.save();
			} },
	        .default_bool = default_config.enable_stream_gui,
	});

	ui::page_header(_cS("page header title", "System"), _cS("page header suibtitle", "Language and advanced options."));
	render_settings(ctx, "##system", list);

	if (disable_instream_gui)
		ImGui::OpenPopup("confirm disable in stream gui");
}

void settings_theme(const settings_context & ctx)
{
	ui::theme & theme = ui::current();
	auto & config = ctx.config;
	auto & default_config = ctx.default_config;

	ui::page_header(_cS("page header title", "Theme"), _cS("page header subtitle", "Accent color, palette and sizing of the interface."));

	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ui::metrics::card_item_spacing);
	const float control_w = ui::metrics::setting_control_width;

	ui::begin_card("##theme");
	{
		// Accent
		ImGui::TextUnformatted(_cS("setting name", "Accent color"));
		ImGui::Dummy({0, 2});
		for (const auto & swatch: ui::accent_swatches())
		{
			const bool selected = theme.accent.x == swatch.base.x and theme.accent.y == swatch.base.y and theme.accent.z == swatch.base.z;
			const auto flags = ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder | (selected ? 0 : ImGuiColorEditFlags_NoDragDrop);
			if (ImGui::ColorButton(swatch.name, swatch.base, flags, {ImGui::GetFrameHeight() * 1.4f, ImGui::GetFrameHeight() * 1.4f}))
			{
				ui::set_accent(swatch);
				config.theme_accent = swatch.name;
				config.save();
			}
			ImGui::SameLine();
		}
		ImGui::NewLine();

		ui::row_separator();

		// Preset
		const auto preset_list = ui::presets();
		std::vector<ui::combo_item> preset_items;
		for (const auto & p: preset_list)
			preset_items.push_back({p.localized_name.c_str()});

		// seed the selection from the saved preset so the box matches what is applied
		auto f = [&](const configuration & cfg) {
			for (int i = 0; i < int(preset_list.size()); ++i)
				if (preset_list[i].name == cfg.theme_preset)
					return i;
			return 1; // dark_default
		};
		static int preset = f(config);
		static const int preset_default = f(default_config);

		ui::setting_label(_cS("setting name", "Preset"), _cS("setting description", "Surface and background palette"), control_w);
		if (ui::combo("##preset", _("Theme preset"), preset_items, &preset, control_w, &preset_default))
		{
			// keep the accent across a preset change, only the surfaces swap
			const ImVec4 keep_accent = theme.accent;
			const ImVec4 keep_hover = theme.accent_hovered;
			const ImVec4 keep_active = theme.accent_active;
			ui::set_theme(preset_list[preset]);
			theme.accent = keep_accent;
			theme.accent_hovered = keep_hover;
			theme.accent_active = keep_active;
			config.theme_preset = preset_list[preset].name;
			config.save();
		}

		ui::row_separator();

		// Rounding
		static const int rounding_default = default_config.theme_rounding;
		int rounding = int(theme.rounding);
		ui::setting_label(_cS("setting label", "Rounding"), _cS("setting description", "Corner radius of controls"), control_w);
		if (ui::slider_int("##rounding", &rounding, 0, 20, "%d px", {control_w, 0}, &rounding_default))
		{
			theme.rounding = float(rounding);
			config.theme_rounding = theme.rounding;
			config.save();
		}

		ui::row_separator();

		static const int card_rounding_default = 14;
		int card_rounding = int(theme.card_rounding);
		ui::setting_label(_cS("setting label", "Card rounding"), _cS("setting description", "Corner radius of panels"), control_w);
		if (ui::slider_int("##card_rounding", &card_rounding, 0, 28, "%d px", {control_w, 0}, &card_rounding_default))
		{
			theme.card_rounding = float(card_rounding);
			config.theme_card_rounding = theme.card_rounding;
			config.save();
		}

		ui::row_separator();

		// global font scale, 100% is the design default
		static const int font_scale_default = 100;
		int font_scale = int(theme.font_scale * 100);
		ui::setting_label(_cS("setting name", "Text size"), _cS("setting description", "Global font scale"), control_w);
		if (ui::slider_int("##font_scale", &font_scale, 60, 140, "%d%%", {control_w, 0}, &font_scale_default))
		{
			theme.font_scale = float(font_scale) / 100.f;
			config.theme_font_scale = theme.font_scale;
			config.save();
		}

		ui::row_separator();

		// Panel transparency, independent of the selected preset
		static const int opacity_default = int(default_config.theme_background_alpha * 100 + 0.5);
		int opacity = int(ui::background_alpha() * 100 + 0.5);
		ui::setting_label(_cS("setting name", "Panel opacity"), _cS("setting description", "Opacity of the panel and card backgrounds"), control_w);
		if (ui::slider_int("##opacity", &opacity, 20, 100, "%d%%", {control_w, 0}, &opacity_default))
		{
			ui::background_alpha() = float(opacity) / 100.f;
			config.theme_background_alpha = ui::background_alpha();
			config.save();
		}

		ui::end_card();
	}

	// Space-lobby visuals. Purely client-local, no server involvement.
	ui::begin_card("##lobby_visuals");
	{
		bool animated = config.animated_lobby;
		const bool animated_default = default_config.animated_lobby;
		ui::setting_label(_cS("setting name", "Animated lobby"), _cS("setting description", "Subtle motion in the space lobby: the starfield drifts, stars twinkle, the gas giant turns and the odd comet passes by."), control_w);
		if (ui::toggle("##animated_lobby", &animated, &animated_default))
		{
			config.animated_lobby = animated;
			config.save();
		}

		ui::row_separator();

		bool warp = config.warp_transition;
		const bool warp_default = default_config.warp_transition;
		ui::setting_label(_cS("setting name", "Warp transition"), _cS("setting description", "Stars stretch into a hyperspace tunnel when connecting to a server, and settle back when you return to the lobby."), control_w);
		if (ui::toggle("##warp_transition", &warp, &warp_default))
		{
			config.warp_transition = warp;
			config.save();
		}

		ui::end_card();
	}

	ImGui::PopStyleVar(); // ImGuiStyleVar_ItemSpacing
}

} // namespace wivrn::gui
