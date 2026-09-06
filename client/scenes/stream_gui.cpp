/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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

#define IMGUI_DEFINE_MATH_OPERATORS

#include "stream.h"

#include "fps_window.h"

#include "application.h"
#include "configuration.h"
#include "constants.h"
#include "gui_common.h"
#include "gui_settings.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "implot.h"
#include "is_finite.h"
#include "render/ui_theme.h"
#include "render/ui_widgets.h"
#include "transport_rates.h"
#include "decoder/nxwarp/nxwarp_decoder.h"
#include "utils/i18n.h"
#include "utils/ranges.h"
#include <IconsFontAwesome6.h>
#include <chrono>
#include <cmath>
#include <glm/ext.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/matrix_access.hpp>
#include <limits>
#include <ranges>
#include <spdlog/spdlog.h>
#include <openxr/openxr.h>

namespace
{
float compute_plot_max_value(float * data, int count, ptrdiff_t stride)
{
	float max = 0;
	uintptr_t ptr = (uintptr_t)data;
	for (int i = 0; i < count; i++)
	{
		max = std::max(max, *(float *)(ptr + i * stride));
	}

	// First power of 10 less than the max
	float x = pow(10, floor(log10(max)));
	return ceil(max / x) * x;
}

std::pair<float, std::string> compute_plot_unit(float max_value)
{
	if (max_value > 1e9)
		return {1e-9, "G"};
	if (max_value > 1e6)
		return {1e-6, "M"};
	if (max_value > 1e3)
		return {1e-3, "k"};
	if (max_value > 1)
		return {1, ""};
	if (max_value > 1e-3)
		return {1e3, "m"};
	if (max_value > 1e-6)
		return {1e6, "u"};
	return {1e9, "n"};
}

struct getter_data
{
	uintptr_t data;
	int stride;
	float multiplier;
};

ImPlotPoint getter(int index, void * data_)
{
	getter_data & data = *(getter_data *)data_;

	return ImPlotPoint(index, *(float *)(data.data + index * data.stride) * data.multiplier);
}
} // namespace

// The user-facing name of a negotiated codec, shared by every place the GUI names one.
static const char * codec_label(wivrn::video_codec c)
{
	switch (c)
	{
		case wivrn::video_codec::h264:
			return "H.264";
		case wivrn::video_codec::h265:
			return "HEVC";
		case wivrn::video_codec::av1:
			return "AV1";
		case wivrn::video_codec::raw:
			return "Raw";
		case wivrn::video_codec::nxwarp:
			return "NX Warp";
	}
	return "?";
}

void scenes::stream::accumulate_metrics(XrTime predicted_display_time, const std::array<std::shared_ptr<shard_accumulator::blit_handle>, decoder_count> & blit_handles, const gpu_timestamps & timestamps)
{
	// On a wall clock of its own, so 60 s of transport history is 60 s whatever the
	// framerate is doing, and unaffected by the guard below
	accumulate_transport_metrics(predicted_display_time);
	// Same reasoning: a rate in frames per second has to be sampled on a clock, and it
	// must keep being sampled through the dt guard below or a stall would freeze the
	// figure that exists to show the stall.
	accumulate_fps(predicted_display_time);

	uint64_t rx = network_session->bytes_received();
	uint64_t tx = network_session->bytes_sent();

	float dt = (predicted_display_time - last_metric_time) * 1e-9f;

	// Sometimes the render function can be called with almost the same predicted_display_time,
	// which can cause issues with the bandwidth estimation.
	if (dt < 0.001f)
		return;

	bandwidth_rx = 0.8 * bandwidth_rx + 0.2 * float(rx - bytes_received) / dt;
	bandwidth_tx = 0.8 * bandwidth_tx + 0.2 * float(tx - bytes_sent) / dt;

	// Filter more aggressively for the compact view
	compact_bandwidth_rx = 0.99 * compact_bandwidth_rx + 0.01 * float(rx - bytes_received) / dt;
	compact_bandwidth_tx = 0.99 * compact_bandwidth_tx + 0.01 * float(tx - bytes_sent) / dt;
	compact_cpu_time = 0.99 * compact_cpu_time + 0.01 * application::get_cpu_time().count() * 1e-9f;
	compact_gpu_time = 0.99 * compact_gpu_time + 0.01 * timestamps.gpu_time;

	last_metric_time = predicted_display_time;
	bytes_received = rx;
	bytes_sent = tx;

	*(gpu_timestamps *)&global_metrics[metrics_offset] = timestamps;
	global_metrics[metrics_offset].cpu_time = application::get_cpu_time().count() * 1e-9f;
	global_metrics[metrics_offset].bandwidth_rx = bandwidth_rx * 8;
	global_metrics[metrics_offset].bandwidth_tx = bandwidth_tx * 8;

	std::vector<shard_accumulator::blit_handle *> active_handles;
	active_handles.reserve(blit_handles.size());
	for (const auto & h: blit_handles)
	{
		if (h)
			active_handles.push_back(h.get());
	}

	if (decoder_metrics.size() != active_handles.size())
		decoder_metrics.resize(active_handles.size());

	auto min_encode_begin = std::numeric_limits<decltype(active_handles[0]->feedback.encode_begin)>::max();
	for (const auto & bh: active_handles)
	{
		if (bh)
			min_encode_begin = std::min(min_encode_begin, bh->feedback.encode_begin);
	}

	for (auto && [metrics, bh]: std::views::zip(decoder_metrics, active_handles))
	{
		if (metrics.size() != global_metrics.size())
			metrics.resize(global_metrics.size());
		if (not bh)
			continue;

		// clang-format off
		metrics[metrics_offset] = bh ? decoder_metric{
			.encode_begin          = (bh->feedback.encode_begin          - min_encode_begin) * 1e-9f,
			.encode_end            = (bh->feedback.encode_end            - min_encode_begin) * 1e-9f,
			.send_begin            = (bh->feedback.send_begin            - min_encode_begin) * 1e-9f,
			.send_end              = (bh->feedback.send_end              - min_encode_begin) * 1e-9f,
			.received_first_packet = (bh->feedback.received_first_packet - min_encode_begin) * 1e-9f,
			.received_last_packet  = (bh->feedback.received_last_packet  - min_encode_begin) * 1e-9f,
			.sent_to_decoder       = (bh->feedback.sent_to_decoder       - min_encode_begin) * 1e-9f,
			.received_from_decoder = (bh->feedback.received_from_decoder - min_encode_begin) * 1e-9f,
			.blitted               = (bh->feedback.blitted               - min_encode_begin) * 1e-9f,
			.displayed             = (bh->feedback.displayed             - min_encode_begin) * 1e-9f,
			.predicted_display     = (bh->view_info.display_time         - min_encode_begin) * 1e-9f,
		}: decoder_metric{};
		// clang-format on
	}

	metrics_offset = (metrics_offset + 1) % global_metrics.size();
}

// --- Frame rate readout -----------------------------------------------------------------
//
// Three numbers people actually need under the latency figure: how many frames the render
// thread put on the panel, how many the decoders produced, and — on NX Warp, whose decoder
// runs on the GPU and can fall well under the panel rate — what its own counters say about
// where the missing frames went.
//
// The rates are differences of monotonic counters over a rolling one second window,
// resampled every constants::stream::fps_sample_period. A one-second window is what makes
// a figure sitting at 27 readable instead of a number that flickers between 24 and 31 at
// panel rate.
//
// The server's own encode rate is not shown: to_headset::transport_status carries the
// bitrate, not a frame rate, and inventing one from timestamps that crossed the link would
// be a guess dressed as a measurement. What the NX Warp line does show is the rate frames
// arrive and close on this headset, which is the closest honest thing to it.

float scenes::stream::panel_refresh_rate() const
{
	const XrDuration period = real_display_period;
	return period > 0 ? float(1e9 / double(period)) : 0.f;
}

ImVec4 scenes::stream::fps_colour(float rate) const
{
	const wivrn::ui::theme & t = wivrn::ui::current();
	const float refresh = panel_refresh_rate();
	if (refresh <= 0)
		return t.text_muted;
	if (rate >= refresh - 0.5f)
		return t.text_muted;
	if (rate < refresh * 0.5f)
		return t.warning;
	return t.text;
}

void scenes::stream::accumulate_fps(XrTime now)
{
	if (fps_last_sample and now - fps_last_sample < constants::stream::fps_sample_period)
		return;
	fps_last_sample = now;

	fps_counters c;
	c.t = now;
	c.displayed = displayed_frames;
	c.iterations = render_iterations;
	c.period_ns = render_period_ns;
	c.pose_age_ns = pose_age_ns;
	c.pose_age_frames = pose_age_frames;
	// Per eye, and only from a stream that carries one: decoded_frames counts every
	// stream, so a base layer stream in a view's slot would otherwise be read out as that
	// eye's frame rate. Its slot stays at zero, which is what it already does on a stereo
	// session where the second stream is silent.
	for (size_t i = 0; i < view_count; ++i)
		c.decoded[i] = is_view(i) ? decoded_frames[i].load(std::memory_order_relaxed) : 0;

	bool nxwarp = false;
#if WIVRN_USE_NXWARP
	// The NX Warp counters, summed over the eye streams: both eyes run the same frame
	// stride, so the arrival and decode rates are the same number twice, while a hole or
	// a late drop is a real loss on whichever stream it happened to.
	{
		// render() already holds decoder_mutex shared for the whole frame, and this runs
		// inside it: taking it again would be a recursive shared lock, which is exactly
		// what deadlocks against a waiting writer.
		float ms = 0;
		float gpu_ms = 0, pass_a_ms = 0, pass_b_ms = 0, bytes = 0, arrival_ms = 0;
		float pass_w_ms = 0, pass_b_skip_ms = 0, pass_b_coded_ms = 0, pass_b_dir_ms = 0;
		float tiles_skip = 0, tiles_coded = 0, tiles_dir = 0;
		bool pass_segments = false;
		uint32_t stride = 1, width = 0, height = 0;
		uint64_t tools = 0;
		uint32_t frame_tiles = 0, grid_tiles = 0;
		for (size_t i = 0; i < view_count; ++i)
		{
			// Eye streams only. The base layer is hardware HEVC, so the cast below
			// would fail on it anyway, but its bytes and decode times are not part of
			// what this block reports (which is the NX Warp codec's own cost per eye)
			// and summing them in would overstate both.
			if (not is_view(i) or not decoders[i].decoder)
				continue;
			auto * d = dynamic_cast<wivrn::nxwarp_decoder *>(decoders[i].decoder->get_decoder().get());
			if (not d)
				continue;
			nxwarp = true;
			const auto st = d->stats();
			c.nxwarp_closed += st.frames_closed;
			c.nxwarp_decoded += st.frames_decoded;
			c.nxwarp_late += st.frames_dropped_late;
			c.nxwarp_holes += st.frames_dropped_holes;
			c.nxwarp_withheld += st.frames_withheld;
			ms = std::max(ms, st.decode_wall_ms);
			// The window figures are per eye and the two eyes run the same stride on
			// the same GPU, so the worse of them is the one that sets the frame rate --
			// the same reason decode_wall_ms above is a max and not a mean.
			gpu_ms = std::max(gpu_ms, st.decode_gpu_ms);
			pass_a_ms = std::max(pass_a_ms, st.decode_pass_a_ms);
			pass_b_ms = std::max(pass_b_ms, st.decode_pass_b_ms);
			// Taken from the SAME eye as the envelope would be, in the sense that
			// matters here: each is the worse of the two, because the eyes run the
			// same stride on the same GPU and the slower one sets the frame rate.
			// That is what the wall and pass figures above already do.
			pass_w_ms = std::max(pass_w_ms, st.decode_pass_w_ms);
			pass_b_skip_ms = std::max(pass_b_skip_ms, st.decode_pass_b_skip_ms);
			pass_b_coded_ms = std::max(pass_b_coded_ms, st.decode_pass_b_coded_ms);
			pass_b_dir_ms = std::max(pass_b_dir_ms, st.decode_pass_b_dir_ms);
			// Tile counts are per eye and describe that eye's work, so the max is
			// the right pick beside the time it explains.
			tiles_skip = std::max(tiles_skip, st.tiles_skip_seg);
			tiles_coded = std::max(tiles_coded, st.tiles_coded_seg);
			tiles_dir = std::max(tiles_dir, st.tiles_dir_seg);
			pass_segments = pass_segments or st.pass_segments_known;

			// Hand this eye's window to the server the first time we see it, so the
			// dashboard can show what the HUD shows. Keyed on the decoder's own
			// window sequence rather than a timer here: it sends each published
			// window exactly once, and it sends nothing at all while the decoder is
			// idle -- which a timer would not manage.
			//
			// This block runs whether or not the overlay is visible (accumulate_fps
			// is called from accumulate_metrics every frame and throttles itself), so
			// the dashboard does not depend on somebody looking at the HUD.
			if (st.window_seq != nxwarp_profile_seq[i])
			{
				nxwarp_profile_seq[i] = st.window_seq;
				send_nxwarp_decode_profile(from_headset::nxwarp_decode_profile{
				        .stream_item_idx = uint8_t(i),
				        .segments_known = st.pass_segments_known,
				        .pass_a_ms = st.decode_pass_a_ms,
				        .pass_b_ms = st.decode_pass_b_ms,
				        .pass_w_ms = st.decode_pass_w_ms,
				        .pass_b_skip_ms = st.decode_pass_b_skip_ms,
				        .pass_b_coded_ms = st.decode_pass_b_coded_ms,
				        .pass_b_dir_ms = st.decode_pass_b_dir_ms,
				        .tiles_skip = st.tiles_skip_seg,
				        .tiles_coded = st.tiles_coded_seg,
				        .tiles_dir = st.tiles_dir_seg,
				});
			}
			// Bytes are per eye and both eyes are coded the same way, so their sum is
			// what the link actually carries per displayed frame.
			bytes += st.bytes_per_frame;
			arrival_ms = std::max(arrival_ms, st.arrival_ms);
			// Fixed for the stream; identical on both eyes, so last writer wins.
			stride = st.decode_stride;
			if (st.encoded_width)
			{
				width = st.encoded_width;
				height = st.encoded_height;
				tools = st.stream_tools;
			}
			// Both eyes are mapped the same way, so last writer wins -- and with the
			// eyes paired there is only one stream reporting at all.
			if (st.grid_tiles)
			{
				frame_tiles = st.frame_tiles;
				grid_tiles = st.grid_tiles;
			}
		}
		// Not a rate: the mean of the decoder's own last two-second profile window,
		// taken as it is rather than differenced.
		fps.nxwarp_ms = ms;
		fps.nxwarp_gpu_ms = gpu_ms;
		fps.nxwarp_pass_a_ms = pass_a_ms;
		fps.nxwarp_pass_b_ms = pass_b_ms;
		fps.nxwarp_pass_w_ms = pass_w_ms;
		fps.nxwarp_pass_b_skip_ms = pass_b_skip_ms;
		fps.nxwarp_pass_b_coded_ms = pass_b_coded_ms;
		fps.nxwarp_pass_b_dir_ms = pass_b_dir_ms;
		fps.nxwarp_tiles_skip = tiles_skip;
		fps.nxwarp_tiles_coded = tiles_coded;
		fps.nxwarp_tiles_dir = tiles_dir;
		fps.nxwarp_pass_segments = pass_segments;
		fps.nxwarp_bytes = bytes;
		fps.nxwarp_arrival_ms = arrival_ms;
		fps.nxwarp_stride = stride;
		fps.nxwarp_width = width;
		fps.nxwarp_height = height;
		// NXVC_TOOL_ENTROPY_LITE, bit 30. Named here rather than including nxvc.h for
		// one constant in a file that otherwise does not need the codec's headers.
		fps.nxwarp_entropy_lite = (tools & (uint64_t(1) << 30)) != 0;
		// From the stream description rather than the codec's own header: the server
		// states it when the decoders are built, where the .nxv header does not arrive
		// until the first frame. See scenes::stream::eyes_in_one_stream().
		fps.nxwarp_paired = eyes_in_one_stream();
		fps.nxwarp_frame_tiles = frame_tiles;
		fps.nxwarp_grid_tiles = grid_tiles;
	}
#endif
	fps.nxwarp = nxwarp;

	// The oldest snapshot still inside the window, which is the one this rate is measured
	// against. Until the ring has filled, the oldest one there is.
	const size_t oldest = fps_ring_count < fps_ring_size
	                              ? (fps_ring_head + fps_ring_size - fps_ring_count) % fps_ring_size
	                              : fps_ring_head;
	if (fps_ring_count > 0)
	{
		const fps_counters & p = fps_ring[oldest];
		const double dt = double(now - p.t) * 1e-9;
		// The window's own rules live in fps_window.h, next to the reasoning for them.
		// The one that matters here: this sampler runs from the render loop, and the
		// render loop stops entirely while the OpenXR session is not running, so a
		// window can be left open across minutes of a headset sitting off the face.
		switch (wivrn::client::judge_window(dt))
		{
			case wivrn::client::window_verdict::stale:
				// Not a slow loop, a gap in the sampling. Nothing measured across
				// it is worth showing, so the ring starts again from here.
				fps_ring_count = 0;
				fps_ring_head = 0;
				break;

			case wivrn::client::window_verdict::too_short:
				break;

			case wivrn::client::window_verdict::usable: {
				const auto rate = [dt](uint64_t a, uint64_t b) {
					return wivrn::client::rate_over(a, b, dt);
				};
				fps.displayed = rate(c.displayed, p.displayed);
				for (size_t i = 0; i < view_count; ++i)
					fps.decoded[i] = rate(c.decoded[i], p.decoded[i]);
				fps.nxwarp_closed = rate(c.nxwarp_closed, p.nxwarp_closed);
				fps.nxwarp_decoded = rate(c.nxwarp_decoded, p.nxwarp_decoded);
				fps.nxwarp_late = rate(c.nxwarp_late, p.nxwarp_late);
				fps.nxwarp_holes = rate(c.nxwarp_holes, p.nxwarp_holes);
				fps.nxwarp_withheld = rate(c.nxwarp_withheld, p.nxwarp_withheld);
				fps.loop_rate = rate(c.iterations, p.iterations);
				fps.display_period_ms = wivrn::client::mean_period_ms(
				        c.period_ns, p.period_ns, c.iterations, p.iterations);
				fps.pose_age_ms = wivrn::client::mean_period_ms(
				        c.pose_age_ns, p.pose_age_ns, c.pose_age_frames, p.pose_age_frames);
				break;
			}
		}
	}

	fps_ring[fps_ring_head] = c;
	fps_ring_head = (fps_ring_head + 1) % fps_ring_size;
	fps_ring_count = std::min(fps_ring_count + 1, fps_ring_size);

	// The one place the strings are built. See rebuild_fps_lines().
	rebuild_fps_lines();
}

// Built once per sample window, from accumulate_fps. Never called from the draw path: both
// places that show these lines draw the cached strings, so a formatting cost that used to be
// paid twice per frame is now paid four times a second, and only when a number moved.
//
// The NX Warp block is four lines under the shown/decoded one, in the order a question gets
// asked: how many frames am I getting, what is the decode costing, what is on the wire, and
// what was negotiated. Everything on them comes from the decoder's own two-second profile
// window or from monotonic counters differenced over one second.
void scenes::stream::rebuild_fps_lines()
{
	for (auto & line: fps_line_cache)
		line.clear();

	// One figure per stream that actually decodes. With the eyes paired there is only
	// stream 0 -- stream 1 has no decoder and its counter never moves -- so listing it
	// would print a permanent "/0" and read as a dead eye rather than an absent stream.
	std::string decoded = fmt::format("{:.0f}", fps.decoded[0]);
	if (not fps.nxwarp_paired)
	{
		for (size_t i = 1; i < view_count; ++i)
			decoded += fmt::format("/{:.0f}", fps.decoded[i]);
	}

	fps_line_cache[0] = fps.nxwarp_paired
	                            ? fmt::format(_F("Shown {:.0f} · decoded {} fps, both eyes"),
	                                          fps.displayed,
	                                          decoded)
	                            : fmt::format(_F("Shown {:.0f} · decoded {} fps"), fps.displayed, decoded);

	// The render loop itself, beside the frames it produced. "Shown" is the subset of
	// these iterations that reached a submission, so the two sitting apart is the whole
	// diagnosis: 33 shown out of 90 is a submission being skipped, 33 out of 33 is the
	// loop arriving late, and the period says what the runtime was aiming for while it
	// happened. Suppressed until the window has a period to report -- a "0.0 ms" panel
	// would be read as a measurement rather than as the absence of one.
	if (fps.display_period_ms > 0)
		fps_line_cache[0] += fmt::format(_F(" · loop {:.0f}/s · period {:.1f} ms"),
		                                 fps.loop_rate,
		                                 fps.display_period_ms);

	// How old the pose reaching the panel is. Beside the loop rate because the two are
	// read together: the same loop rate with a smaller age is the schedule working, and
	// a smaller age bought by a slower loop is not a win at all. Suppressed when no frame
	// in the window carried one, for the same reason the period above is.
	if (fps.pose_age_ms > 0)
		fps_line_cache[0] += fmt::format(_F(" · pose age {:.1f} ms"), fps.pose_age_ms);

	if (not fps.nxwarp)
		return;

	// Where the frames went. "in" is what the reassembler closed off the wire, which is the
	// server's send rate as seen from here; late and holes are the two ways one is lost, and
	// "held" is not a loss at all -- the frame decoded and was withheld on purpose. Every
	// field is always drawn, including the zeroes: a block whose fields appear and disappear
	// reflows under the reader, and on a HUD that is worse than a column of noughts.
	fps_line_cache[1] = fmt::format(
	        _F("NX Warp  in {:.0f} · dec {:.0f} · late {:.0f} · holes {:.0f} · held {:.0f} /s"),
	        fps.nxwarp_closed,
	        fps.nxwarp_decoded,
	        fps.nxwarp_late,
	        fps.nxwarp_holes,
	        fps.nxwarp_withheld);

	// What a decode costs. The two passes are the whole reason this line exists: pass B
	// scales with the pixel count, so it is the half the stream scale moves, and seeing it
	// next to the wall figure is what tells a slow session from a saturated one.
	// B here is the ENVELOPE -- Pass A's end to Pass B's end -- which is what nxvc
	// reports and what this line has always shown. What it did not say is that the
	// envelope contains the predictor dispatch and three separate reconstruction
	// segments, so "A 7.6 / B 23.1" read as "the reconstruct kernel costs 23 ms".
	// The line below breaks it up; this one keeps the envelope, because the envelope
	// is the number that has to fit in the frame period.
	fps_line_cache[2] = fmt::format(_F("decode {:.1f} ms · GPU {:.1f} (A {:.1f} / B {:.1f})"),
	                                fps.nxwarp_ms,
	                                fps.nxwarp_gpu_ms,
	                                fps.nxwarp_pass_a_ms,
	                                fps.nxwarp_pass_b_ms);

	// What is on the wire, and how often. Stride 1 means every arriving frame is decoded;
	// above 1 the decoder is skipping, which is the number that explains a decode rate
	// sitting at half the arrival rate.
	fps_line_cache[3] = fmt::format(_F("{:.1f} kB/frame · arriving every {:.1f} ms · stride {}"),
	                                fps.nxwarp_bytes / 1024.f,
	                                fps.nxwarp_arrival_ms,
	                                fps.nxwarp_stride);

	// What was negotiated, which does not change once the stream is up but is the first
	// thing anyone asks when the numbers above look wrong. Paired, the size shown is the
	// frame the decoder actually dispatches -- the eyes side by side -- because that is
	// what the decode figures on the line above were measured on.
	if (fps.nxwarp_width)
	{
		// The stream header reports the size PER EYE. Paired, the picture the decoder
		// actually dispatches is the eyes side by side, so that is what is shown: it is
		// the frame the GPU figures on the line above were measured on, and a "1088x1088"
		// next to a 49 ms decode would invite exactly the wrong conclusion.
		const uint32_t eyes = fps.nxwarp_paired ? 2u : 1u;
		const uint32_t w = fps.nxwarp_width * eyes;
		const uint32_t tiles = (w / 64) * (fps.nxwarp_height / 64);
		// std::string, not const char *: _S is gettext(...).c_str() over a temporary
		// std::string, so a pointer to it dangles the moment this statement ends. Inline
		// inside the format call it would live long enough; hoisted, it does not.
		const std::string entropy = fps.nxwarp_entropy_lite ? _("entropy lite") : _("entropy rANS");
		// Which mapping the server is sending under, told from what arrived rather
		// than from anything on the wire saying so: under the fixed-chunk mapping a
		// frame is a PREFIX of the tile grid and carries one tile per MTU-sized piece
		// of its bitstream, and under per-tile spans it carries every tile it coded.
		// The two are not close -- 45 against 578 on a paired 1088x1088 -- so the test
		// is a wide margin and not a threshold to tune. Blank until a frame has closed.
		std::string mapping;
		if (fps.nxwarp_grid_tiles and fps.nxwarp_frame_tiles)
			mapping = fps.nxwarp_frame_tiles * 2 >= fps.nxwarp_grid_tiles
			                  ? fmt::format(_F(" · spans, {} tiles/frame"), fps.nxwarp_frame_tiles)
			                  : fmt::format(_F(" · chunks, {} tiles/frame"), fps.nxwarp_frame_tiles);
		fps_line_cache[4] = fps.nxwarp_paired
		                            ? fmt::format(_F("{}x{} paired · {} tiles · {}{}"),
		                                          w,
		                                          fps.nxwarp_height,
		                                          tiles,
		                                          entropy,
		                                          mapping)
		                            : fmt::format(_F("{}x{} · {} tiles · {}{}"),
		                                          w,
		                                          fps.nxwarp_height,
		                                          tiles,
		                                          entropy,
		                                          mapping);
	}

	// What pass B is actually made of.
	//
	// The parts are shown as an equation that SUMS, because the point of the line is
	// that the envelope is not one kernel. "warp" is Pass W, the predictor dispatch;
	// "skip" is the WARP_SKIP tiles, which run the normative integer pose warp
	// themselves and are usually the largest term by a wide margin; "coded" is every
	// other non-INTRA tile and "dir" the directional-intra wavefront.
	//
	// "other" is the remainder and is left visible on purpose. The four parts are
	// timestamps taken around dispatches, so the pipeline drain between them belongs
	// to none of them -- it is what the segmentation itself costs. Folding it into
	// the parts would make them add up by making them wrong; dropping it would leave
	// an equation that visibly does not balance and invite the reader to assume a
	// bug. Naming it is the only option that is both honest and readable.
	//
	// Absent entirely when the numbers are not measured -- an nxvc without the
	// segment timers, or a device with no timestamp support -- rather than shown as
	// a convincing row of zeroes.
	if (fps.nxwarp_pass_segments and fps.nxwarp_pass_b_ms > 0)
	{
		const float parts = fps.nxwarp_pass_w_ms + fps.nxwarp_pass_b_skip_ms +
		                    fps.nxwarp_pass_b_coded_ms + fps.nxwarp_pass_b_dir_ms;
		// Clamped at zero: the envelope and the segments are separate timestamp
		// pairs, so at the noise floor of a very cheap frame the parts can total a
		// hair over the envelope, and a HUD is not the place to show -0.0.
		const float other = std::max(0.f, fps.nxwarp_pass_b_ms - parts);
		fps_line_cache[5] = fmt::format(
		        _F("B {:.1f} = warp {:.1f} · skip {:.1f} · coded {:.1f} · dir {:.1f} · other {:.1f} ms"),
		        fps.nxwarp_pass_b_ms,
		        fps.nxwarp_pass_w_ms,
		        fps.nxwarp_pass_b_skip_ms,
		        fps.nxwarp_pass_b_coded_ms,
		        fps.nxwarp_pass_b_dir_ms,
		        other);
		// The tile counts that explain the times: a segment at 0 ms with no tiles was
		// empty, one at 0 ms with tiles is a device that cannot time it.
		if (fps.nxwarp_tiles_skip or fps.nxwarp_tiles_coded or fps.nxwarp_tiles_dir)
			fps_line_cache[5] += fmt::format(_F(" · tiles {:.0f}/{:.0f}/{:.0f}"),
			                                 fps.nxwarp_tiles_skip,
			                                 fps.nxwarp_tiles_coded,
			                                 fps.nxwarp_tiles_dir);
	}

	// The performance levels: what we ASKED the runtime for, and the last thing
	// it told us it did.  The two halves are separate on purpose --
	// xrPerfSettingsSetPerformanceLevelEXT succeeding says nothing about
	// whether the level was honoured, and the notification is the only thing
	// that ever contradicts it.  Absent entirely when the extension is not
	// there, rather than shown as a confident "sustained_low".
	{
		// `session` is the scene's own reference (scene::session).
		if (session.has_performance_settings())
		{
			const char * cpu = session.applied_valid[0]
			                           ? xr::to_string(session.applied_level[0])
			                           : "?";
			const char * gpu = session.applied_valid[1]
			                           ? xr::to_string(session.applied_level[1])
			                           : "?";
			fps_line_cache[6] = fmt::format(_F("perf asked CPU {} · GPU {}"), cpu, gpu);
			if (const auto e = application::get_last_perf_event())
				fps_line_cache[6] += fmt::format(
				        _F(" · runtime {}/{} {} → {}"),
				        xr::to_string(e->domain), xr::to_string(e->subDomain),
				        xr::to_string(e->fromLevel), xr::to_string(e->toLevel));
		}
		else
			fps_line_cache[6].clear();
	}
}

// The cached block, drawn. Both views call this so they cannot drift apart.
//
// Only the two rate lines are coloured against the panel refresh: the rest are costs and
// negotiated facts, and colouring a millisecond figure by a frame rate threshold would be
// a red number that means nothing.
void scenes::stream::draw_fps_lines()
{
	const wivrn::ui::theme & t = wivrn::ui::current();
	const auto & lines = fps_lines();
	for (size_t i = 0; i < lines.size(); ++i)
	{
		if (lines[i].empty())
			continue;
		if (i == 0)
			ImGui::TextColored(fps_colour(std::min(fps.displayed, fps.decoded[0])), "%s", lines[i].c_str());
		else if (i == 1)
			ImGui::TextColored(fps_colour(fps.nxwarp_decoded), "%s", lines[i].c_str());
		else
			ImGui::TextColored(t.text_muted, "%s", lines[i].c_str());
	}
}

void scenes::stream::gui_performance_metrics()
{
	const ImGuiStyle & style = ImGui::GetStyle();
	const wivrn::ui::theme & t = wivrn::ui::current();

	ImVec2 window_size = ImGui::GetWindowSize() - ImVec2(2, 2) * style.WindowPadding;

	const std::array plots = {
	        // clang-format off
	        plot(_("CPU time"), {{"",          &global_metric::cpu_time}},     "s"),

	        plot(_("GPU time"), {{_("Defoveate"), &global_metric::gpu_time}},  "s"),

	        plot(_("Network"), {{_("Download"),  &global_metric::bandwidth_rx},
	                            {_("Upload"),    &global_metric::bandwidth_tx}}, "bit/s"),
	        // clang-format on
	};

	int n_plots = plots.size() + decoder_metrics.size();
	axis_scale.resize(n_plots);

	int n_cols = 2;
	int n_rows = ceil((float)n_plots / n_cols);

	ImVec2 plot_size = ImVec2(
	        window_size.x / n_cols - style.ItemSpacing.x * (n_cols - 1) / n_cols,
	        (window_size.y - 2 * ImGui::GetCurrentContext()->FontSize - 2 * style.ItemSpacing.y) / n_rows - style.ItemSpacing.y * (n_rows - 1) / n_rows);

	ImPlot::PushStyleColor(ImPlotCol_PlotBg, ImVec4{t.background.x, t.background.y, t.background.z, 0.8f});
	ImPlot::PushStyleColor(ImPlotCol_PlotBorder, t.border);
	ImPlot::PushStyleColor(ImPlotCol_LegendBg, ImVec4{t.card.x, t.card.y, t.card.z, 0.9f});
	ImPlot::PushStyleColor(ImPlotCol_InlayText, t.text);
	ImPlot::PushStyleColor(ImPlotCol_FrameBg, IM_COL32(0, 0, 0, 0));
	ImPlot::PushStyleColor(ImPlotCol_AxisBg, IM_COL32(0, 0, 0, 0));
	ImPlot::PushStyleColor(ImPlotCol_AxisBgActive, IM_COL32(0, 0, 0, 0));
	ImPlot::PushStyleColor(ImPlotCol_AxisBgHovered, IM_COL32(0, 0, 0, 0));

	int n = 0;
	for (const auto & [title, subplots, unit]: plots)
	{
		if (ImPlot::BeginPlot(title.c_str(), plot_size, ImPlotFlags_NoTitle | ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoMouseText))
		{
			float min_v = 0;
			float max_v = 0;
			for (const auto & [subtitle, data]: subplots)
			{
				max_v = std::max(max_v, compute_plot_max_value(&(global_metrics.data()->*data), global_metrics.size(), sizeof(global_metric)));
			}
			auto [multiplier, prefix] = compute_plot_unit(max_v);

			if (axis_scale[n] == 0 || not wivrn::is_finite(axis_scale[n]))
				axis_scale[n] = max_v;
			else
				axis_scale[n] = 0.99 * axis_scale[n] + 0.01 * max_v;

			auto color = ImPlot::GetColormapColor(n);

			std::string title_with_units = std::string(title) + " [" + prefix + unit + "]";
			ImPlot::SetupAxes(nullptr, title_with_units.c_str(), ImPlotAxisFlags_NoDecorations, 0);
			ImPlot::SetupAxesLimits(0, global_metrics.size() - 1, min_v * multiplier, axis_scale[n] * multiplier, ImGuiCond_Always);
			ImPlot::SetNextLineStyle(color);
			ImPlot::SetNextFillStyle(color, 0.25);

			for (const auto & [subtitle, data]: subplots)
			{
				getter_data gdata{
				        .data = (uintptr_t)&(global_metrics.data()->*data),
				        .stride = sizeof(global_metric),
				        .multiplier = multiplier,
				};
				ImPlot::PlotLineG(subtitle.c_str(), getter, &gdata, global_metrics.size(), ImPlotLineFlags_Shaded);

				double x[] = {double(metrics_offset), double(metrics_offset)};
				double y[] = {0, axis_scale[n] * multiplier};
				ImPlot::SetNextLineStyle(ImVec4(1, 1, 1, 1));
				ImPlot::PlotLine("", x, y, 2);
			}
			ImPlot::EndPlot();
		}

		if (++n % n_cols != 0)
			ImGui::SameLine();
	}

	for (auto && [index, metrics]: utils::enumerate(decoder_metrics))
	{
		std::string title = fmt::format(_F("Decoder {}"), std::to_string(index));
		if (ImPlot::BeginPlot(title.c_str(), plot_size, ImPlotFlags_NoTitle | ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoMouseText))
		{
			float min_v = 0;
			float max_v = compute_plot_max_value(&(metrics.data()->displayed), metrics.size(), sizeof(decoder_metric));
			// auto [ multiplier, prefix ] = compute_plot_unit(max_v);

			if (axis_scale[n] == 0)
				axis_scale[n] = max_v;
			else
				axis_scale[n] = 0.99 * axis_scale[n] + 0.01 * max_v;

			std::string title_with_units = _("Timings [ms]");
			ImPlot::SetupAxes(nullptr, title_with_units.c_str(), ImPlotAxisFlags_NoDecorations, 0);
			ImPlot::SetupAxesLimits(0, metrics.size() - 1, min_v * 1e3f, axis_scale[n] * 1e3f, ImGuiCond_Always);

			getter_data getter_encode_begin{
			        .data = (uintptr_t)&(metrics.data()->encode_begin),
			        .stride = sizeof(decoder_metric),
			        .multiplier = 1e3f};

			getter_data getter_encode_end{
			        .data = (uintptr_t)&(metrics.data()->encode_end),
			        .stride = sizeof(decoder_metric),
			        .multiplier = 1e3f};

			getter_data getter_send_begin{
			        .data = (uintptr_t)&(metrics.data()->send_begin),
			        .stride = sizeof(decoder_metric),
			        .multiplier = 1e3f};

			getter_data getter_send_end{
			        .data = (uintptr_t)&(metrics.data()->send_end),
			        .stride = sizeof(decoder_metric),
			        .multiplier = 1e3f};

			getter_data getter_received_first_packet{
			        .data = (uintptr_t)&(metrics.data()->received_first_packet),
			        .stride = sizeof(decoder_metric),
			        .multiplier = 1e3f};

			getter_data getter_received_last_packet{
			        .data = (uintptr_t)&(metrics.data()->received_last_packet),
			        .stride = sizeof(decoder_metric),
			        .multiplier = 1e3f};

			getter_data getter_sent_to_decoder{
			        .data = (uintptr_t)&(metrics.data()->sent_to_decoder),
			        .stride = sizeof(decoder_metric),
			        .multiplier = 1e3f};

			getter_data getter_received_from_decoder{
			        .data = (uintptr_t)&(metrics.data()->received_from_decoder),
			        .stride = sizeof(decoder_metric),
			        .multiplier = 1e3f};

			getter_data getter_blitted{
			        .data = (uintptr_t)&(metrics.data()->blitted),
			        .stride = sizeof(decoder_metric),
			        .multiplier = 1e3f};

			getter_data getter_displayed{
			        .data = (uintptr_t)&(metrics.data()->displayed),
			        .stride = sizeof(decoder_metric),
			        .multiplier = 1e3f};

			getter_data getter_predicted{
			        .data = (uintptr_t)&(metrics.data()->predicted_display),
			        .stride = sizeof(decoder_metric),
			        .multiplier = 1e3f};

			// clang-format off
			ImPlot::PlotShadedG(_S("Encode"),  getter, &getter_encode_begin,          getter, &getter_encode_end,            metrics.size());
			ImPlot::PlotShadedG(_S("Send"),    getter, &getter_send_begin,            getter, &getter_send_end,              metrics.size());
			ImPlot::PlotShadedG(_S("Receive"), getter, &getter_received_first_packet, getter, &getter_received_last_packet,  metrics.size());
			ImPlot::PlotShadedG(_S("Decode"),  getter, &getter_sent_to_decoder,       getter, &getter_received_from_decoder, metrics.size());
			ImPlot::PlotLineG(_S("Blitted"),   getter, &getter_blitted,                                                      metrics.size());
			ImPlot::PlotLineG(_S("Displayed"), getter, &getter_displayed,                                                    metrics.size());
			ImPlot::PlotLineG(_S("Predicted"), getter, &getter_predicted,                                                    metrics.size());
			// clang-format on

			double x[] = {double(metrics_offset), double(metrics_offset)};
			double y[] = {0, 1e9};
			ImPlot::SetNextLineStyle(ImVec4(1, 1, 1, 1));
			ImPlot::PlotLine("", x, y, 2);

			ImPlot::EndPlot();
		}

		if (++n % n_cols != 0)
			ImGui::SameLine();
	}

	ImPlot::PopStyleColor(8);
	{
		ImGui::TextUnformatted(
		        fmt::format(
		                _F("Estimated motion to photons latency: {}ms"),
		                tracking_control.lock()->motions_to_photons / 1'000'000)
		                .c_str());

		// Directly under the latency figure: what the panel is actually being shown and
		// what the decoders are actually producing, then the NX Warp block. The same
		// lines the compact view draws.
		draw_fps_lines();

		if (is_gui_interactable())
			ImGui::Text("%s", _S("Press the grip button to move the window"));
		else
			ImGui::Text("%s", _S("Press both thumbsticks to display the WiVRn window"));
	}
}

void scenes::stream::on_wifi_sample(bool valid, int rssi_dbm, int link_speed_mbps)
{
	radio_valid = valid;
	if (not valid)
		return;

	radio_rssi_dbm = rssi_dbm;
	radio_link_speed_mbps = link_speed_mbps;

	// Two averages of the same series at different rates: the fast one follows the signal,
	// the slow one lags behind it, and the gap between them is the direction it is moving.
	// Steadier than differencing consecutive samples and cheaper than the regression the
	// server runs — the page only needs an arrow, not a slope in dB/s.
	//
	// Zero is the "no sample yet" sentinel: a real RSSI is negative, never 0.
	const float fast = radio_rssi_fast;
	if (fast == 0)
	{
		radio_rssi_fast = float(rssi_dbm);
		radio_rssi_slow = float(rssi_dbm);
		return;
	}

	radio_rssi_fast = wivrn::ema_step(fast, float(rssi_dbm), constants::stream::radio_fast_alpha);
	radio_rssi_slow = wivrn::ema_step(radio_rssi_slow, float(rssi_dbm), constants::stream::radio_slow_alpha);
}

void scenes::stream::accumulate_transport_metrics(XrTime predicted_display_time)
{
	// Every stream rebuilds its own shards; the page shows the link, not the eye.
	// decoder_mutex is already held, shared, by render() for the whole frame.
	auto reconstructed_total = [this] {
		uint64_t total = 0;
		for (const auto & d: decoders)
		{
			if (d.decoder)
				total += d.decoder->reconstructed_shards();
		}
		return total;
	};

	if (transport_last_sample == 0)
	{
		// A rate needs two readings; the first one only opens the window
		transport_last_sample = predicted_display_time;
		transport_bytes_received = network_session->bytes_received();
		transport_reconstructed = reconstructed_total();
		transport_concealments = audio_handle ? audio_handle->concealment_events() : 0;
		return;
	}

	const XrDuration dt = predicted_display_time - transport_last_sample;
	if (dt < constants::stream::transport_sample_period)
		return;

	const uint64_t rx = network_session->bytes_received();
	const uint64_t fec = reconstructed_total();
	const uint64_t concealed = audio_handle ? audio_handle->concealment_events() : 0;

	// The setpoint holds its last value while the server is quiet: a line falling to zero
	// would read as the bitrate collapsing, which is not what a missing status packet means.
	const size_t previous = (transport_offset + transport_metrics.size() - 1) % transport_metrics.size();
	float setpoint = transport_metrics[previous].setpoint_bps;
	if (auto status = transport_status.lock();
	    status->has_value() and predicted_display_time - transport_status_received < constants::stream::transport_status_stale)
		setpoint = float((*status)->bitrate_bps);

	transport_metrics[transport_offset] = transport_metric{
	        .video_bps = float(wivrn::counter_rate(transport_bytes_received, rx, dt) * 8),
	        .setpoint_bps = setpoint,
	        .primary_rtt_s = float(primary_rtt_ns) * 1e-9f,
	        .secondary_rtt_s = float(secondary_rtt_ns) * 1e-9f,
	        .fec_per_s = float(wivrn::counter_rate(transport_reconstructed, fec, dt)),
	        .conceal_per_min = float(wivrn::counter_rate(transport_concealments, concealed, dt) * 60),
	};
	transport_offset = (transport_offset + 1) % transport_metrics.size();

	transport_last_sample = predicted_display_time;
	transport_bytes_received = rx;
	transport_reconstructed = fec;
	transport_concealments = concealed;
}

void scenes::stream::gui_transport()
{
	const wivrn::ui::theme & t = wivrn::ui::current();
	const configuration & config = application::get_config();
	const XrTime now = instance.now();

	// Renew the lease on the server's status feed. It is a lease and not a switch on
	// purpose: the server drops the feed by itself once it lapses, so a page that stops
	// being drawn — including because the headset went away mid-frame — stops the traffic
	// whether or not the client ever gets to say so.
	if (transport_status_next_req == 0 or now >= transport_status_next_req)
	{
		transport_status_next_req = now + XrDuration(std::chrono::nanoseconds(wivrn::transport_status_refresh).count());
		network_session->send_control(from_headset::transport_status_subscribe{.active = true});
	}

	std::optional<to_headset::transport_status> status;
	{
		auto locked = transport_status.lock();
		if (locked->has_value() and now - transport_status_received < constants::stream::transport_status_stale)
			status = *locked;
	}

	const size_t newest = (transport_offset + transport_metrics.size() - 1) % transport_metrics.size();
	const transport_metric & latest = transport_metrics[newest];

	wivrn::ui::page_header(_cS("page header title", "Transport"),
	                       _cS("page header subtitle", "Link, bitrate control and error recovery, live."));

	// muted caption on the left, value right-aligned, one line
	auto stat = [&t](const std::string & label, const std::string & value, const ImVec4 & color) {
		ImGui::PushStyleColor(ImGuiCol_Text, t.text_muted);
		ImGui::TextUnformatted(label.c_str());
		ImGui::PopStyleColor();
		ImGui::SameLine();
		const float w = ImGui::CalcTextSize(value.c_str()).x;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x - w));
		ImGui::PushStyleColor(ImGuiCol_Text, color);
		ImGui::TextUnformatted(value.c_str());
		ImGui::PopStyleColor();
	};

	struct series
	{
		float transport_metric::* data;
		ImVec4 color;
	};

	// 60 s of history, no decorations: at arm's length in VR the shape is the message and
	// tick labels are unreadable anyway, so the unit goes in the axis title instead.
	auto history_plot = [&](const char * id, const char * unit, int scale_index, const std::vector<series> & plots) {
		const float height = ImGui::GetFrameHeight() * 3.2f;
		if (not ImPlot::BeginPlot(id, {-1, height}, ImPlotFlags_NoTitle | ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoMouseText | ImPlotFlags_NoLegend))
			return;

		float max_v = 0;
		for (const auto & p: plots)
			max_v = std::max(max_v, compute_plot_max_value(&(transport_metrics.data()->*p.data), transport_metrics.size(), sizeof(transport_metric)));

		auto [multiplier, prefix] = compute_plot_unit(max_v);

		float & scale = transport_axis_scale[scale_index];
		if (scale == 0 or not wivrn::is_finite(scale))
			scale = max_v;
		else
			scale = 0.9f * scale + 0.1f * max_v;

		const std::string axis = std::string(prefix) + unit;
		ImPlot::SetupAxes(nullptr, axis.c_str(), ImPlotAxisFlags_NoDecorations, 0);
		ImPlot::SetupAxesLimits(0, transport_metrics.size() - 1, 0, std::max(scale * multiplier, 1e-3f), ImGuiCond_Always);

		for (const auto & p: plots)
		{
			getter_data gdata{
			        .data = (uintptr_t)&(transport_metrics.data()->*p.data),
			        .stride = sizeof(transport_metric),
			        .multiplier = multiplier,
			};
			ImPlot::SetNextLineStyle(p.color);
			ImPlot::SetNextFillStyle(p.color, 0.2f);
			ImPlot::PlotLineG("", getter, &gdata, transport_metrics.size(), ImPlotLineFlags_Shaded);
		}

		// Where the ring wraps, i.e. the present
		double x[] = {double(transport_offset), double(transport_offset)};
		double y[] = {0, 1e12};
		ImPlot::SetNextLineStyle(t.border);
		ImPlot::PlotLine("", x, y, 2);

		ImPlot::EndPlot();
	};

	transport_axis_scale.resize(3);

	ImPlot::PushStyleColor(ImPlotCol_PlotBg, IM_COL32(0, 0, 0, 0));
	ImPlot::PushStyleColor(ImPlotCol_PlotBorder, IM_COL32(0, 0, 0, 0));
	ImPlot::PushStyleColor(ImPlotCol_InlayText, t.text_muted);
	ImPlot::PushStyleColor(ImPlotCol_FrameBg, IM_COL32(0, 0, 0, 0));
	ImPlot::PushStyleColor(ImPlotCol_AxisText, t.text_muted);
	ImPlot::PushStyleColor(ImPlotCol_AxisBg, IM_COL32(0, 0, 0, 0));
	ImPlot::PushStyleColor(ImPlotCol_AxisBgActive, IM_COL32(0, 0, 0, 0));
	ImPlot::PushStyleColor(ImPlotCol_AxisBgHovered, IM_COL32(0, 0, 0, 0));

	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, wivrn::ui::metrics::card_item_spacing);

	if (ImGui::BeginTable("transport", 2, ImGuiTableFlags_SizingStretchSame))
	{
		// --- Link -------------------------------------------------------------------
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		wivrn::ui::begin_card("##link");
		{
			ImGui::TextUnformatted(_S("Link"));
			ImGui::Dummy({0, 2});

			using path_state = to_headset::transport_status::path_state;
			const bool on_usb = status ? status->path == path_state::usb : network_session->sending_on_secondary();
			const bool combining = status and status->path == path_state::combining;
			const bool usb_ready = status ? status->path != path_state::wifi_only : network_session->has_secondary();

			wivrn::ui::chip(combining ? _("Wi-Fi + USB") : on_usb ? _("USB backup")
			                                                      : _("Wi-Fi"),
			                on_usb ? wivrn::ui::chip_style::warning : wivrn::ui::chip_style::success,
			                true);
			ImGui::SameLine();
			if (combining)
				// Share of the video bytes each link actually took over the last
				// status period, which is the only honest way to say how much the
				// aggregation is really buying.
				wivrn::ui::chip(fmt::format(_F("{}% / {}% split"),
				                            int(status->wifi_share_pct),
				                            int(100 - status->wifi_share_pct)),
				                wivrn::ui::chip_style::accent);
			else if (usb_ready and not on_usb)
				wivrn::ui::chip(_("USB standing by"), wivrn::ui::chip_style::muted);
			else if (not usb_ready)
				wivrn::ui::chip(_("no backup path"), wivrn::ui::chip_style::muted);
			ImGui::SameLine();
			wivrn::ui::chip(config.wifi_qos ? _("QoS on") : _("QoS off"),
			                config.wifi_qos ? wivrn::ui::chip_style::accent : wivrn::ui::chip_style::muted);

			ImGui::Dummy({0, 4});

			auto rtt = [&](int64_t ns) {
				return ns > 0 ? fmt::format("{:.1f} ms", float(ns) * 1e-6f) : std::string("—");
			};
			// Both paths carry video while combining, so neither is the muted one
			stat(_("Wi-Fi RTT"), rtt(primary_rtt_ns), on_usb ? t.text_muted : t.text);
			stat(_("USB RTT"), rtt(secondary_rtt_ns), (on_usb or combining) ? t.text : t.text_muted);

			history_plot("##rtt", "s", 0, {{&transport_metric::primary_rtt_s, t.accent}, {&transport_metric::secondary_rtt_s, t.success}});

			if (radio_valid)
			{
				// The trend is the gap between the two averages, not a slope: rising
				// means the signal is getting better than it has recently been.
				const int trend = wivrn::trend_direction(radio_rssi_fast, radio_rssi_slow, constants::stream::radio_trend_deadband_db);
				const char * arrow = trend > 0 ? ICON_FA_ARROW_UP : trend < 0 ? ICON_FA_ARROW_DOWN
				                                                              : ICON_FA_ARROW_RIGHT;
				const ImVec4 & color = trend > 0 ? t.success : trend < 0 ? t.warning
				                                                         : t.text;

				stat(_("Signal"), fmt::format("{} {} dBm", arrow, int(radio_rssi_dbm)), color);
				if (radio_link_speed_mbps > 0)
					stat(_("PHY rate"), fmt::format("{} Mbit/s", int(radio_link_speed_mbps)), t.text);
			}
			else
			{
				stat(_("Signal"), _("unavailable"), t.text_muted);
			}
		}
		wivrn::ui::end_card();

		// --- Bitrate ----------------------------------------------------------------
		ImGui::TableNextColumn();
		wivrn::ui::begin_card("##bitrate");
		{
			ImGui::TextUnformatted(_S("Bitrate"));
			ImGui::Dummy({0, 2});

			// Falls back to what the headset asked for: without the server's answer
			// that is the only bitrate anything is known to be running at.
			const uint32_t bps = status ? status->bitrate_bps : config.bitrate_bps;
			ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * wivrn::ui::metrics::font_title);
			ImGui::PushStyleColor(ImGuiCol_Text, t.accent);
			ImGui::TextUnformatted(fmt::format("{:.1f} Mbit/s", bps / 1e6).c_str());
			ImGui::PopStyleColor();
			ImGui::PopFont();

			using controller_state = to_headset::transport_status::controller_state;
			std::string law = _C("Bitrate control", "Manual");
			std::string state;
			if (status)
			{
				switch (status->state)
				{
					case controller_state::off:
						break;
					case controller_state::steady:
						state = _C("bitrate controller state", "steady");
						break;
					case controller_state::recovering:
						state = _C("bitrate controller state", "recovering");
						break;
					case controller_state::startup:
						state = _C("bitrate controller state", "growing estimate");
						break;
					case controller_state::probe:
						state = _C("bitrate controller state", "probing");
						break;
				}
				if (status->state != controller_state::off)
					law = status->mode == wivrn::bitrate_mode::bbr
					              ? _C("Bitrate control", "Adaptive v2")
					              : _C("Bitrate control", "Adaptive");
			}

			stat(_("Control"), state.empty() ? law : law + " · " + state, t.text);
			stat(_("Ceiling"),
			     status ? fmt::format("{:.0f} Mbit/s", status->ceiling_bps / 1e6) : std::string("—"),
			     t.text_muted);

			if (status and status->radio_hold)
			{
				ImGui::Dummy({0, 2});
				wivrn::ui::chip(wivrn::ui::icon_label(ICON_FA_TOWER_BROADCAST, _("radio hold")),
				                wivrn::ui::chip_style::warning,
				                true);
			}

			// Last automatic resort below the bitrate floor: the stream framerate has been
			// halved to keep the struggling link alive.
			if (status and status->emergency_framerate)
			{
				ImGui::Dummy({0, 2});
				wivrn::ui::chip(wivrn::ui::icon_label(ICON_FA_ARROW_DOWN, _("emergency half-rate")),
				                wivrn::ui::chip_style::warning,
				                true);
			}

			// Server setpoint against what actually arrived: the gap between them is
			// the overhead, and a measured line that will not follow the setpoint up
			// is the link refusing to carry it.
			history_plot("##bitrate", "bit/s", 1, {{&transport_metric::setpoint_bps, t.accent}, {&transport_metric::video_bps, t.text_muted}});

			stat(_("Received"), fmt::format("{:.1f} Mbit/s", latest.video_bps / 1e6), t.text_muted);

			if (not status)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, t.text_muted);
				ImGui::TextUnformatted(_S("The server is not reporting: everything above is what the headset asked for."));
				ImGui::PopStyleColor();
			}
		}
		wivrn::ui::end_card();

		// --- Reliability ------------------------------------------------------------
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		wivrn::ui::begin_card("##reliability");
		{
			ImGui::TextUnformatted(_S("Reliability"));
			ImGui::Dummy({0, 2});

			const bool fec = status ? status->fec_active : config.fec;
			wivrn::ui::chip(fec ? _("FEC on") : _("FEC off"),
			                fec ? wivrn::ui::chip_style::success : wivrn::ui::chip_style::muted,
			                true);

			ImGui::Dummy({0, 4});
			stat(_("Shards rebuilt"), fmt::format("{}", transport_reconstructed), t.text);
			stat(_("Rebuilt now"), fmt::format("{:.1f} /s", latest.fec_per_s), t.text);

			history_plot("##fec", "/s", 2, {{&transport_metric::fec_per_s, t.success}});

			// The headset never asks for a keyframe: it reports the hole and the server
			// decides. This is the count of holes it reported.
			stat(_("Frames lost"), fmt::format("{}", uint64_t(incomplete_frames)), incomplete_frames > 0 ? t.warning : t.text_muted);
			stat(_("Audio gaps concealed"), fmt::format("{}", transport_concealments), t.text_muted);
			stat(_("Audio gaps now"), fmt::format("{:.1f} /min", latest.conceal_per_min), t.text_muted);

			// Purely local: the server is never told about the playout delay and could not
			// use it if it were. A value sitting above zero is the headset saying the frames
			// are arriving unevenly — which nothing else on this page shows, because they
			// all arrived.
			if (config.dejitter)
			{
				const float ms = float(dejitter.delay_ns()) * 1e-6f;
				stat(_("De-jitter buffer"),
				     fmt::format("{:.1f} ms", ms),
				     ms > 0.05f ? t.warning : t.text_muted);
			}
			else
			{
				stat(_("De-jitter buffer"), _("off"), t.text_muted);
			}
		}
		wivrn::ui::end_card();

		// --- Encoders ---------------------------------------------------------------
		ImGui::TableNextColumn();
		wivrn::ui::begin_card("##encoders");
		{
			ImGui::TextUnformatted(_S("Encoders"));
			ImGui::Dummy({0, 2});

			const bool paced = status ? status->pacing_active : false;
			wivrn::ui::chip(paced ? _("pacing on") : _("pacing off"),
			                paced ? wivrn::ui::chip_style::success : wivrn::ui::chip_style::muted,
			                true);
			ImGui::Dummy({0, 4});

			// By role, not by position: the base layer sits in the slot the eye
			// pairing vacates, so naming these by index would list it as "Right".
			// The two views keep their left/right names, which is the one thing a
			// role cannot say -- for those the index IS the view index.
			const auto stream_name = [this](size_t i) -> std::string {
				if (is_alpha(i))
					return _C("video stream", "Alpha");
				if (is_base(i))
					return _C("video stream", "Base layer");
				if (i == quad_stream_idx)
					return _C("video stream", "Quad layer");
				return i == 0 ? _C("video stream", "Left")
				              : _C("video stream", "Right");
			};

			// decoder_mutex is held, shared, by render() around the whole frame,
			// this page included: taking it again here would be a recursive
			// shared lock, which std::shared_mutex does not allow.
			if (not video_stream_description)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, t.text_muted);
				ImGui::TextUnformatted(_S("No video stream yet."));
				ImGui::PopStyleColor();
			}
			for (size_t i = 0; video_stream_description and i < decoder_count; ++i)
			{
				if (not decoders[i].decoder)
					continue;

				const char * codec = codec_label(video_stream_description->codec[i]);

				// A stream the server had to hand to x264 is the one thing on this
				// page a user can act on, so it is the one thing coloured.
				const bool software = status and (status->software_encoders & (1u << i));
				stat(stream_name(i),
				     software ? fmt::format("{} · {}", codec, _("software")) : std::string(codec),
				     software ? t.warning : t.text);
			}
		}
		wivrn::ui::end_card();

		// --- Motion smoothing -------------------------------------------------------
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		wivrn::ui::begin_card("##smoothing");
		{
			ImGui::TextUnformatted(_S("Motion smoothing"));
			ImGui::Dummy({0, 2});

			const auto mode = config.motion_mode();
			const XrTime last = motion_field_last;
			const XrDuration age = last ? now - last : 0;
			// Motion fields only arrive in headset mode, and only when the application
			// falls behind the display, so idle is the normal state and not a fault. In
			// server mode there are no fields at all and the server says whether it is
			// warping; without a status packet nothing here can know.
			const bool active = mode == wivrn::motion_mode::server
			                            ? (status and status->server_warping)
			                            : (mode == wivrn::motion_mode::headset and last and age < 1'000'000'000);

			wivrn::ui::chip(mode == wivrn::motion_mode::off      ? _("off")
			                : mode == wivrn::motion_mode::server ? (active ? _("server · warping") : _("server · idle"))
			                : active                             ? _("warping")
			                                                     : _("idle"),
			                active ? wivrn::ui::chip_style::accent : wivrn::ui::chip_style::muted,
			                true);
			ImGui::Dummy({0, 4});
			stat(_("Fields received"), fmt::format("{}", uint64_t(motion_field_count)), t.text_muted);
			stat(_("Last field"), last ? fmt::format("{:.0f} ms ago", age * 1e-6) : std::string("—"), t.text_muted);
		}
		wivrn::ui::end_card();

		ImGui::TableNextColumn();

		ImGui::EndTable();
	}

	ImGui::PopStyleVar();
	ImPlot::PopStyleColor(8);
}

void scenes::stream::gui_compact_view()
{
	const auto & metrics = global_metrics[(metrics_offset + global_metrics.size() - 1) % global_metrics.size()];

	if (ImGui::BeginTable("metrics", 2))
	{
		auto f = [&](const char * label, float value, const char * unit) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("%s", label);
			ImGui::TableNextColumn();
			ImGui::Text("%.1f %s", value, unit);
		};

		f(_S("Download"), 8 * compact_bandwidth_rx * 1e-6, "Mbit/s");
		f(_S("Upload"), 8 * compact_bandwidth_tx * 1e-6, "Mbit/s");
		f(_S("CPU time"), compact_cpu_time * 1000, "ms");
		f(_S("GPU time"), compact_gpu_time * 1000, "ms");
		f(_S("Motion to photon latency"),
		  tracking_control.lock()->motions_to_photons / 1'000'000.f,
		  "ms");
		ImGui::EndTable();
	}

	// Directly under the latency figure, outside the two-column table so the block keeps
	// its own width and does not stretch the compact panel.
	draw_fps_lines();
}

static void send_settings_changed_packet(xr::session & session, wivrn_session * network, const configuration & config)
{
	network->send_control(
	        from_headset::settings_changed{
	                .preferred_refresh_rate = config.preferred_refresh_rate,
	                .minimum_refresh_rate = config.minimum_refresh_rate.value_or(0),
	                .fps_divider = config.fps_divider,
	                .bitrate_bps = config.bitrate_bps,
	                .bitrate_auto = config.bitrate_auto,
	                .bitrate_control = config.bitrate_control(),
	                .radio_aware = config.radio_aware,
	                .smooth_pacing = config.smooth_pacing,
	                .fec = config.fec,
	                .fec_adaptive = config.fec_adaptive,
	                .shard_retransmit = config.shard_retransmit,
	                .wifi_qos = config.wifi_qos,
	                .sharp_text = config.sharp_text,
	                .encoder_failover = config.encoder_failover,
	                .intra_refresh = config.intra_refresh,
	                .ref_invalidation = config.ref_invalidation,
	                .emergency_framerate = config.emergency_framerate,
	                .motion_smoothing = config.motion_smoothing,
	                .motion_smoothing_mode = config.motion_mode(),
	                .multipath = config.multipath_mode(),
	                .quad_layers = config.quad_layers,
	                .low_latency_audio = config.low_latency_audio,
	                .standby_freeze = config.standby_freeze,
	                .mirror_gamepad = config.forward_gamepad,
	                .enabled_body_parts = config.body_part_mask,
	                // Stored for the next connection; the encoded size cannot change live.
	                .render_scale = config.effective_render_scale(),
	                // Foveation v2: applied live by the server, recomputed per frame.
	                .foveation_strength = config.effective_foveation_strength(),
	                .foveation_adaptive = config.foveation_adaptive,
	                .foveation_foveal_qp = config.foveation_foveal_qp,
	        });
}

void scenes::stream::gui_settings(float)
{
	// same pages as the lobby, with in_game enabling the in-stream controls
	wivrn::gui::settings_context ctx{
	        .config = application::get_config(),
	        .default_config = application::get_default_config(),
	        .instance = instance,
	        .session = session,
	        .system = system,
	        .imgui_ctx = *imgui_ctx,
	        .recommended_width = width,
	        .recommended_height = height,
	        .in_game = true,
	        .server_hid_forwarding = hid_forwarding_enabled(),
	        .on_streaming_changed = [this] { send_settings_changed_packet(session, network_session.get(), application::get_config()); },
	        .enter_foveation_adjust = [this] { next_gui_status = stream_tab::foveation_settings; },
	        .on_foveation_override_changed = [this] {
		        const auto & config = application::get_config();
		        override_foveation_enable = config.override_foveation_enable;
		        override_foveation_pitch = config.override_foveation_pitch;
		        override_foveation_distance = config.override_foveation_distance;
		        network_session->send_control(from_headset::override_foveation_center{
		                .enabled = override_foveation_enable,
		                .pitch = override_foveation_pitch,
		                .distance = override_foveation_distance,
		        }); },
	        .on_qos_changed = [this] {
		        network_session->set_qos(application::get_config().wifi_qos);
		        send_settings_changed_packet(session, network_session.get(), application::get_config()); },
	        .on_audio_path_changed = [this] {
		        // Each end routes what it sends: the packet tells the server, the
		        // handle tells our own microphone callback
		        if (audio_handle)
			        audio_handle->set_low_latency(application::get_config().low_latency_audio);
		        send_settings_changed_packet(session, network_session.get(), application::get_config()); },
	};

	switch (current_settings_page)
	{
		case settings_page::video:
			wivrn::gui::settings_video(ctx);
			break;
		case settings_page::audio:
			wivrn::gui::settings_audio(ctx);
			break;
		case settings_page::streaming:
			wivrn::gui::settings_streaming(ctx);
			break;
		case settings_page::post_processing:
			wivrn::gui::settings_post_processing(ctx);
			break;
		case settings_page::devices:
			wivrn::gui::settings_devices(ctx);
			break;
		case settings_page::tracking:
			if (wivrn::gui::settings_tracking(ctx))
				send_settings_changed_packet(session, network_session.get(), ctx.config);
			break;
		case settings_page::system:
			wivrn::gui::settings_system(ctx);
			break;
		case settings_page::theme:
			wivrn::gui::settings_theme(ctx);
			break;
	}
}

void scenes::stream::gui_bitrate_settings(float predicted_display_period)
{
	auto & config = application::get_config();
	ImGui::PushFont(nullptr, constants::gui::font_size_large);
	ImGui::Text("%s", _S("Use the right thumbstick to adjust the bitrate"));
	ImGui::Text("%s", _S("Press A to go back"));
	ImGui::Text("%s", fmt::format(_F("Bitrate: {}Mbit/s"), config.bitrate_bps / 1'000'000).c_str());
	ImGui::PopFont();

	// Maximum speed of 20Mbit/s
	float delta = application::read_action_float(settings_adjust).value_or(std::pair{0, 0}).second * 20'000'000.f * predicted_display_period;

	config.bitrate_bps = std::clamp(config.bitrate_bps + static_cast<int32_t>(delta), 1'000'000u, config.max_bitrate());

	bool ok = application::read_action_bool(foveation_ok).value_or(std::pair{0, false}).second;

	if (ok)
	{
		config.save();
		next_gui_status = stream_tab::settings;
	}

	send_settings_changed_packet(session, network_session.get(), application::get_config());
}

void scenes::stream::gui_foveation_settings(float predicted_display_period)
{
	ImGui::PushFont(nullptr, constants::gui::font_size_large);
	ImGui::Text("%s", _S("Use the thumbsticks to move the foveation center"));
	ImGui::Text("%s", _S("Press A to save or B to cancel"));
	ImGui::Text("%s", fmt::format(_F("Height {:.1f} °"), -override_foveation_pitch * 180 / M_PI).c_str());
	ImGui::Text("%s", fmt::format(_F("Distance {:.2f} m"), override_foveation_distance).c_str());
	ImGui::PopFont();

	// Maximum speed 1 rad/s
	float delta_pitch = application::read_action_float(settings_adjust).value_or(std::pair{0, 0}).second * predicted_display_period;

	// Maximum speed 2m/s @ 1m
	float delta_distance = std::pow(constants::stream::gui_max_foveation_speed, application::read_action_float(foveation_distance).value_or(std::pair{0, 0}).second * predicted_display_period);

	override_foveation_pitch = std::clamp<float>(override_foveation_pitch + delta_pitch, constants::stream::gui_min_foveation_pitch, constants::stream::gui_max_foveation_pitch);
	override_foveation_distance = std::clamp<float>(override_foveation_distance * delta_distance, constants::stream::gui_min_foveation_distance, constants::stream::gui_max_foveation_distance);

	bool ok = application::read_action_bool(foveation_ok).value_or(std::pair{0, false}).second;
	bool cancel = application::read_action_bool(foveation_cancel).value_or(std::pair{0, false}).second;

	if (ok)
	{
		next_gui_status = stream_tab::settings;

		// Save settings
		auto & config = application::get_config();
		config.override_foveation_enable = true;
		config.override_foveation_pitch = override_foveation_pitch;
		config.override_foveation_distance = override_foveation_distance;
		config.save();
	}
	else if (cancel)
	{
		next_gui_status = stream_tab::settings;

		// Restore settings
		const auto & config = application::get_config();
		override_foveation_enable = config.override_foveation_enable;
		override_foveation_pitch = config.override_foveation_pitch;
		override_foveation_distance = config.override_foveation_distance;
	}

	network_session->send_control(from_headset::override_foveation_center{
	        .enabled = override_foveation_enable,
	        .pitch = override_foveation_pitch,
	        .distance = override_foveation_distance,
	});
}

void scenes::stream::gui_applications()
{
	auto now = instance.now();
	if (now - running_application_req > 1'000'000'000)
	{
		running_application_req = now;
		network_session->send_control(from_headset::get_running_applications{});
	}

	wivrn::ui::page_header(_cS("page header title", "Applications"), _cS("page header subtitle", "Running XR applications on the server."));

	auto apps = running_applications.lock();
	std::ranges::sort(apps->applications, [](auto & l, auto & r) {
		if (l.overlay == r.overlay)
			return false;
		return r.overlay;
	});

	const float gap = ImGui::GetStyle().ItemSpacing.x;
	const float ctrl_h = ImGui::GetFrameHeight() * wivrn::ui::metrics::control_height;
	const std::string stop_label = wivrn::ui::icon_label(ICON_FA_XMARK, _C("button label to ask an application to quit", "Stop"));
	const float stop_w = wivrn::ui::button_width(stop_label);
	const std::string active_label = wivrn::ui::icon_label(ICON_FA_CIRCLE_CHECK, _C("chip displayed next to a running application while streaming", "Active"));
	const float active_w = wivrn::ui::chip_width(active_label, false, stop_w);

	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, wivrn::ui::metrics::card_item_spacing);
	wivrn::ui::begin_list_card("##running");
	{
		if (apps->applications.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, wivrn::ui::current().text_muted);
			ImGui::TextUnformatted(_S("No XR application is currently running."));
			ImGui::PopStyleColor();
		}

		bool overlay = false;
		bool first = true;
		for (const auto & app: apps->applications)
		{
			if (app.overlay and not overlay)
			{
				overlay = true;
				ImGui::PushStyleColor(ImGuiCol_Text, wivrn::ui::current().text_muted);
				ImGui::TextUnformatted(_S("Overlays"));
				ImGui::PopStyleColor();
				first = true;
			}
			ImGui::PushID(static_cast<int>(app.id));
			if (not first)
				wivrn::ui::row_separator();
			first = false;

			// overlays and the active app aren't selectable, only their stop button acts
			const bool interactive = not(app.active or app.overlay);
			const float trailing = stop_w + (app.active ? gap + active_w : 0) + wivrn::ui::metrics::list_row_pad;
			const auto row = wivrn::ui::begin_list_row("##row", ICON_FA_CUBE, 0, app.name, {}, app.active, trailing, 0, false, interactive);
			float x = row.max.x;

			ImGui::SetCursorScreenPos(row.trailing(x, {stop_w, ctrl_h}));
			if (wivrn::ui::button(stop_label, wivrn::ui::button_style::danger, {stop_w, 0}))
				network_session->send_control(from_headset::stop_application{.id = app.id});
			if (ImGui::IsItemHovered())
				imgui_ctx->tooltip(_S("Request to quit, may be ignored by the application"));
			x -= stop_w + gap;

			if (app.active)
			{
				ImGui::SetCursorScreenPos(row.trailing(x, {active_w, ctrl_h}));
				wivrn::ui::chip(active_label, wivrn::ui::chip_style::success, false, ctrl_h);
			}

			if (row.clicked and interactive)
				network_session->send_control(from_headset::set_active_application{.id = app.id});

			wivrn::ui::end_list_row();
			ImGui::PopID();
		}
	}
	wivrn::ui::end_card();
	ImGui::PopStyleVar();
}

void scenes::stream::gui_toasts()
{
	auto toast = gui_toast.lock();

	if (!toast->has_value())
	{
		ImGui::Text("%s", _S("Press both thumbsticks to display the WiVRn window"));
		return;
	}

	ImGui::Text("%s", (*toast)->content.c_str());
}

void scenes::stream::draw_gui(XrTime predicted_display_time, XrDuration predicted_display_period)
{
	if (auto new_status = next_gui_status.load(); new_status != gui_status)
	{
		spdlog::info("Switch tab from {} to {}", magic_enum::enum_name(gui_status), magic_enum::enum_name(new_status));

		if (not is_gui_interactable() and is_interactable(new_status))
		{
			if (auto head_position = application::locate_controller(application::space(xr::spaces::view), application::space(xr::spaces::world), predicted_display_time))
			{
				world_gui_orientation = head_position->second * head_gui_orientation;
				world_gui_position = head_position->first + glm::mat3_cast(head_position->second) * head_gui_position;
			}
		}
		else if (is_gui_interactable() and not is_interactable(new_status))
		{
			if (auto head_position = application::locate_controller(application::space(xr::spaces::view), application::space(xr::spaces::world), predicted_display_time))
			{
				head_gui_orientation = glm::conjugate(head_position->second) * world_gui_orientation;
				head_gui_position = glm::mat3_cast(glm::conjugate(head_position->second)) * (world_gui_position - head_position->first);
			}
		}

		// The status feed is only useful while its page is on screen. The server drops it
		// on its own once the lease lapses; saying so makes the common case immediate.
		if (gui_status == stream_tab::transport and new_status != stream_tab::transport)
		{
			transport_status_next_req = 0;
			network_session->send_control(from_headset::transport_status_subscribe{.active = false});
		}

		stored_gui_status = gui_status;
		gui_status = new_status;
		gui_status_last_change = predicted_display_time;

		// Override session state if the GUI is interactable
		if (not is_gui_interactable())
			network_session->send_control(from_headset::session_state_changed{
			        .state = application::get_session_state(),
			});
		else if (application::get_session_state() == XR_SESSION_STATE_FOCUSED)
			network_session->send_control(from_headset::session_state_changed{
			        .state = XR_SESSION_STATE_VISIBLE,
			});

		network_session->send_control(from_headset::stream_tab_changed{.tab = new_status});
	}

	bool interactable = true;
	XrSpace world_space = application::space(xr::spaces::world);
	auto views = session.locate_views(viewconfig, predicted_display_time, world_space).second;

	switch (gui_status)
	{
		case stream_tab::hidden:
		case stream_tab::foveation_settings:
		case stream_tab::overlay_only:
		case stream_tab::compact:
			interactable = false;
			break;
		case stream_tab::stats:
		case stream_tab::transport:
		case stream_tab::settings:
		case stream_tab::applications:
		case stream_tab::application_launcher:
			break;
	}
	imgui_ctx->set_controllers_enabled(interactable and not recentering_context);
	if (interactable)
	{
		if (system.hand_tracking_supported())
		{
			if (not left_hand)
				left_hand = session.create_hand_tracker(XR_HAND_LEFT_EXT);
			if (not right_hand)
				right_hand = session.create_hand_tracker(XR_HAND_RIGHT_EXT);
		}
	}
	else
	{
		left_hand.reset();
		right_hand.reset();
	}

	float alpha = 1;
	bool is_urgent = false;
	if (gui_status == stream_tab::hidden)
	{
		auto toast = gui_toast.lock();
		if (toast->has_value())
			is_urgent = (*toast)->is_urgent;

		float t = (predicted_display_time - gui_status_last_change) * 1.e-9f;
		float delay = is_urgent ? constants::stream::urgent_fade_delay : constants::stream::fade_delay;

		alpha = std::clamp<float>(1 - (t - delay) / constants::stream::fade_duration, 0, 1);

		if (alpha == 0)
		{
			toast->reset();
			return;
		}
	}

	// Lock the GUI position to the head, do it before displaying the GUI to avoid being off by one frame when gui_status changes
	std::optional<std::pair<glm::vec3, glm::quat>> head_position = application::locate_controller(application::space(xr::spaces::view), world_space, predicted_display_time);
	if (head_position)
	{
		glm::mat3 M = glm::mat3_cast(head_position->second);
		switch (gui_status)
		{
			case stream_tab::foveation_settings:
				imgui_ctx->layers()[0].orientation = head_position->second;
				imgui_ctx->layers()[0].position = head_position->first + M * glm::vec3{0, override_foveation_distance * sin(override_foveation_pitch), -override_foveation_distance};
				break;

			case stream_tab::hidden:
				// Always use the same position for the GUI shortcut tip
				imgui_ctx->layers()[0].orientation = head_position->second;
				imgui_ctx->layers()[0].position = head_position->first + M * glm::vec3{0.0, -0.4, -1.0};
				break;

			case stream_tab::overlay_only:
			case stream_tab::compact:
				imgui_ctx->layers()[0].orientation = head_position->second * head_gui_orientation;
				imgui_ctx->layers()[0].position = head_position->first + M * head_gui_position;
				break;

			case stream_tab::stats:
			case stream_tab::transport:
			case stream_tab::settings:
			case stream_tab::applications:
			case stream_tab::application_launcher:
				imgui_ctx->layers()[0].orientation = world_gui_orientation;
				imgui_ctx->layers()[0].position = world_gui_position;
				break;
		}
	}

	// popup layer floats in front of the main panel so combos and modals pop as their own quad
	imgui_ctx->place_layer_relative(2, 0, constants::gui::popup_position);

	const float tab_width = wivrn::ui::metrics::sidebar_width;
	const float top_bar_h = wivrn::ui::metrics::top_bar_height;
	const float content_margin = wivrn::ui::metrics::content_margin;
	const ImVec2 margin_around_window{50, 50};

	ImGuiStyle & style = ImGui::GetStyle();
	imgui_ctx->new_frame(predicted_display_time);

	// theme the shared cards like the lobby, widget hooks are global so re-point at this scene
	style.FontScaleMain = wivrn::ui::current().font_scale * wivrn::ui::metrics::font_base;
	style.WindowRounding = wivrn::ui::current().card_rounding;
	style.ChildRounding = wivrn::ui::current().card_rounding;
	style.PopupRounding = wivrn::ui::current().card_rounding;
	style.FrameRounding = wivrn::ui::current().rounding;
	style.GrabRounding = wivrn::ui::current().rounding;
	style.TabRounding = wivrn::ui::current().rounding;
	style.Colors[ImGuiCol_Text] = wivrn::ui::current().text;
	style.Colors[ImGuiCol_TextDisabled] = wivrn::ui::current().text_muted;
	wivrn::ui::set_popup_center(imgui_ctx->layers()[2].vp_center(), float(imgui_ctx->layers()[2].vp_size.y));
	wivrn::ui::set_hover_haptic([this] { imgui_ctx->vibrate_on_hover(); });
	wivrn::ui::set_tooltip_hook([this](const char * text) { imgui_ctx->tooltip(text); });

	ImVec2 viewport_size(imgui_ctx->layers()[0].vp_size.x, imgui_ctx->layers()[0].vp_size.y);
	ImVec2 content_size{viewport_size - ImVec2{tab_width, 0} - margin_around_window * 2};
	ImVec2 content_center = margin_around_window + content_size / 2 + ImVec2{tab_width, 0};

	bool display_tabs = false;
	bool always_auto_resize = false;
	switch (gui_status)
	{
		case stream_tab::overlay_only:
			ImGui::SetNextWindowPos(content_center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			ImGui::SetNextWindowSize(content_size);
			break;

		case stream_tab::hidden:
		case stream_tab::foveation_settings:
			ImGui::SetNextWindowPos(viewport_size / 2, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			always_auto_resize = true;
			break;

		case stream_tab::compact:
			ImGui::SetNextWindowPos(content_center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			always_auto_resize = true;
			break;

		case stream_tab::stats:
		case stream_tab::transport:
		case stream_tab::settings:
		case stream_tab::applications:
			ImGui::SetNextWindowPos(margin_around_window);
			ImGui::SetNextWindowSize(viewport_size - margin_around_window * 2);
			display_tabs = true;
			break;
		case stream_tab::application_launcher:
			ImGui::SetNextWindowPos(margin_around_window);
			ImGui::SetNextWindowSize(viewport_size - margin_around_window * 2);
			break;
	}

	if (is_urgent)
	{
		ImGui::PushStyleColor(ImGuiCol_Border, constants::stream::urgent_border_color);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 4);
	}

	// themed translucent background, matching the lobby
	const wivrn::ui::theme & th = wivrn::ui::current();
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{th.background.x, th.background.y, th.background.z, wivrn::ui::background_alpha()});

	ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0);
	if (always_auto_resize)
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8, 8});
		ImGui::Begin("Compact view", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize);
	}
	else
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
		ImGui::Begin("Stream settings", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
	}

	if (is_urgent)
	{
		ImGui::PopStyleColor(1);
		ImGui::PopStyleVar(1);
	}

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8, 8});
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10);

	switch (gui_status)
	{
		case stream_tab::hidden:
			gui_toasts();
			break;

		case stream_tab::overlay_only:
			ImGui::SetCursorPos({20, 20});
			ImGui::BeginChild("Main", ImVec2(ImGui::GetWindowSize().x - ImGui::GetCursorPosX(), 0));
			gui_performance_metrics();
			ImGui::EndChild();
			break;

		case stream_tab::compact:
			gui_compact_view();
			break;

		case stream_tab::stats:
			ImGui::SetCursorPos({tab_width + content_margin, top_bar_h});
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {20, 20});
			ImGui::BeginChild("Main", ImVec2(ImGui::GetWindowSize().x - ImGui::GetCursorPosX() - content_margin, 0));
			ImGui::SetCursorPosY(20);
			{
				// Name the codec where people look first: nobody should have to open the
				// Transport page to learn whether the session is NX Warp or HEVC.
				std::string subtitle = _("Live streaming performance.");
				if (video_stream_description)
					subtitle = fmt::format("{} {}", subtitle, fmt::format(_F("Video codec: {}."), codec_label(video_stream_description->codec[0])));
				wivrn::ui::page_header(_S("Statistics"), subtitle);
			}
			ImGui::BeginChild("plots", {0, 0});
			gui_performance_metrics();
			ImGui::EndChild();
			ImGui::EndChild();
			ImGui::PopStyleVar();
			break;

		case stream_tab::transport:
			ImGui::SetCursorPos({tab_width + content_margin, top_bar_h});
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {20, 20});
			ImGui::BeginChild("Main", ImVec2(ImGui::GetWindowSize().x - ImGui::GetCursorPosX() - content_margin, 0));
			ImGui::SetCursorPosY(20);
			gui_transport();
			ImGui::Dummy(ImVec2(0, 20));
			ScrollWhenDragging();
			ImGui::EndChild();
			ImGui::PopStyleVar();
			break;

		case stream_tab::settings:
			ImGui::SetCursorPos({tab_width + content_margin, top_bar_h});
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {20, 20});
			ImGui::BeginChild("Main", ImVec2(ImGui::GetWindowSize().x - ImGui::GetCursorPosX() - content_margin, 0));
			ImGui::SetCursorPosY(20);
			gui_settings(predicted_display_period * 1.e-9f);
			ImGui::Dummy(ImVec2(0, 20));
			ScrollWhenDragging();
			ImGui::EndChild();
			ImGui::PopStyleVar();
			break;

		case stream_tab::foveation_settings:
			gui_foveation_settings(predicted_display_period * 1.e-9f);
			break;

		case stream_tab::applications:
			ImGui::SetCursorPos({tab_width + content_margin, top_bar_h});
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {20, 20});
			ImGui::BeginChild("Main", ImVec2(ImGui::GetWindowSize().x - ImGui::GetCursorPosX() - content_margin, 0));
			ImGui::SetCursorPosY(20);
			gui_applications();
			ImGui::Dummy(ImVec2(0, 20));
			ScrollWhenDragging();
			ImGui::EndChild();
			ImGui::PopStyleVar();
			break;

		case stream_tab::application_launcher:
			if (apps.draw_gui(*imgui_ctx, _("Cancel")) != app_launcher::None)
				next_gui_status = stream_tab::applications;
	}

	ImGui::PopStyleVar(2); // ImGuiStyleVar_WindowPadding, ImGuiStyleVar_FrameRounding

	if (display_tabs)
	{
		// top bar: logo left, battery/connection status/window controls right
		const float side = ImGui::GetFrameHeight() * wivrn::ui::metrics::control_height;
		std::vector<wivrn::ui::top_bar_item> top_items;
		if (auto bat = wivrn::gui::battery_status_indicator(instance.now()))
			top_items.push_back({wivrn::ui::chip_width(bat->label, false, side),
			                     [bat = *bat, side] { wivrn::ui::chip(bat.label, bat.style, false, side); }});
		const std::string conn = _C("status in the title bar", "Connected");
		top_items.push_back({wivrn::ui::chip_width(conn, true, side),
		                     [conn, side] { wivrn::ui::chip(conn, wivrn::ui::chip_style::success, true, side); }});
		const std::string close_label = _S("Close");
		top_items.push_back({wivrn::ui::button_width(ICON_FA_XMARK, close_label),
		                     [this, close_label, side] {
			                     if (wivrn::ui::button(ICON_FA_XMARK, close_label, wivrn::ui::button_style::secondary, {0, side}))
				                     next_gui_status = stream_tab::hidden;
		                     }});
		// disconnect asks for confirmation, OpenPopup/confirm_modal share the window id stack
		bool request_disconnect = false;
		const std::string disconnect_label = _S("Disconnect");
		top_items.push_back({wivrn::ui::button_width(ICON_FA_DOOR_OPEN, disconnect_label),
		                     [&request_disconnect, disconnect_label, side] {
			                     if (wivrn::ui::button(ICON_FA_DOOR_OPEN, disconnect_label, wivrn::ui::button_style::danger, {0, side}))
				                     request_disconnect = true;
		                     }});
		wivrn::ui::top_bar(top_bar_h, wivrn_logo, top_items);

		if (request_disconnect)
		{
			network_session->send_control(from_headset::get_running_applications{});
			ImGui::OpenPopup("confirm disconnect");
		}

		if (wivrn::ui::begin_modal("confirm disconnect", _("Disconnect")))
		{
			const std::string kill_apps = ICON_FA_XMARK " " + _("Stop all applications");
			const std::string disconnect_only = ICON_FA_DOOR_OPEN " " + _("Disconnect");
			const std::string cancel = _("Cancel");

			const float gap = ImGui::GetStyle().ItemSpacing.x;

			const float kill_apps_w = ImGui::CalcTextSize(kill_apps.c_str()).x + wivrn::ui::metrics::button_padding.x * 2;
			const float disconnect_only_w = ImGui::CalcTextSize(disconnect_only.c_str()).x + wivrn::ui::metrics::button_padding.x * 2;
			const float cancel_w = ImGui::CalcTextSize(cancel.c_str()).x + wivrn::ui::metrics::button_padding.x * 2;
			const float buttons_width = disconnect_only_w + cancel_w + kill_apps_w + 2 * gap;

			ImGui::Dummy({std::max<float>(500, buttons_width), 0});

			const auto & t = wivrn::ui::current();
			ImGui::PushStyleColor(ImGuiCol_Text, t.text_muted);
			ImGui::TextWrapped("%s", _S("Disconnect from the server and return to the lobby?"));
			ImGui::TextWrapped("%s", _S("The following applications/overlays are open:"));

			for (const auto & app: running_applications.lock()->applications)
				ImGui::TextWrapped("• %s", app.name.c_str());

			ImGui::PopStyleColor();
			ImGui::Dummy({0, 12});

			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttons_width);

			if (button(kill_apps, wivrn::ui::button_style::danger, {kill_apps_w, 0}))
			{
				for (const auto & app: running_applications.lock()->applications)
					network_session->send_control(from_headset::stop_application{.id = app.id});
				exit();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();

			if (button(disconnect_only, wivrn::ui::button_style::primary, {disconnect_only_w, 0}))
			{
				exit();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();

			if (button(cancel, wivrn::ui::button_style::secondary, {cancel_w, 0}))
			{
				ImGui::CloseCurrentPopup();
			}

			wivrn::ui::end_modal();
		}

		// navigation sidebar, the settings items swap the page but keep the coarse settings tab
		wivrn::ui::begin_sidebar(top_bar_h, tab_width, 2);
		{
			wivrn::ui::nav_section(_cS("tab group", "STREAM"));
			if (wivrn::ui::nav_item(ICON_FA_LIST, _cS("tab label", "Applications"), gui_status == stream_tab::applications))
				next_gui_status = stream_tab::applications;
			if (wivrn::ui::nav_item(ICON_FA_ROCKET, _cS("tab label", "Start"), false))
			{
				apps.reset();
				network_session->send_control(from_headset::get_application_list{
				        .language = application::get_messages_info().language,
				        .country = application::get_messages_info().country,
				        .variant = application::get_messages_info().variant,
				});
				next_gui_status = stream_tab::application_launcher;
			}
			if (wivrn::ui::nav_item(ICON_FA_COMPUTER, _S("Statistics"), gui_status == stream_tab::stats))
				next_gui_status = stream_tab::stats;
			if (wivrn::ui::nav_item(ICON_FA_TOWER_BROADCAST, _cS("tab label", "Transport"), gui_status == stream_tab::transport))
				next_gui_status = stream_tab::transport;

			wivrn::ui::nav_section(_cS("tab group", "SETTINGS"));
			auto settings_item = [&](const char * icon, const std::string & label, settings_page page) {
				if (wivrn::ui::nav_item(icon, label, gui_status == stream_tab::settings and current_settings_page == page))
				{
					current_settings_page = page;
					next_gui_status = stream_tab::settings;
				}
			};
			settings_item(ICON_FA_IMAGE, _cS("tab label", "Video"), settings_page::video);
			settings_item(ICON_FA_VOLUME_HIGH, _cS("tab label", "Audio"), settings_page::audio);
			settings_item(ICON_FA_TOWER_BROADCAST, _cS("tab label", "Streaming"), settings_page::streaming);
			settings_item(ICON_FA_WAND_MAGIC_SPARKLES, _cS("tab label", "Post-processing"), settings_page::post_processing);
			settings_item(ICON_FA_KEYBOARD, _cS("tab label", "Devices"), settings_page::devices);
			settings_item(ICON_FA_LOCATION_CROSSHAIRS, _cS("tab label", "Tracking"), settings_page::tracking);
			settings_item(ICON_FA_GEARS, _cS("tab label", "System"), settings_page::system);
			settings_item(ICON_FA_PALETTE, _cS("tab label", "Theme"), settings_page::theme);

			// pinned to the bottom
			wivrn::ui::sidebar_footer();
			if (wivrn::ui::nav_item(ICON_FA_CHART_LINE, _cS("tab label", "Statistics overlay"), false))
				next_gui_status = stream_tab::overlay_only;
			if (wivrn::ui::nav_item(ICON_FA_MINIMIZE, _cS("tab label", "Compact view"), false))
				next_gui_status = stream_tab::compact;
		}
		wivrn::ui::end_sidebar();

		wivrn::ui::shell_dividers(top_bar_h, tab_width);
	}
	ImGui::End();
	ImGui::PopStyleVar(2);  // ImGuiStyleVar_ChildBorderSize, ImGuiStyleVar_WindowPadding
	ImGui::PopStyleColor(); // ImGuiCol_WindowBg

	auto layers = imgui_ctx->end_frame();

	// Display controllers and handle recentering
	if (interactable)
	{
		if (recentering_context)
		{
			xr::spaces controller = std::get<0>(*recentering_context);
			bool state;
			switch (controller)
			{
				case xr::spaces::aim_left:
					state = application::read_action_bool(recenter_left).value_or(std::pair{0, false}).second;
					break;
				case xr::spaces::aim_right:
					state = application::read_action_bool(recenter_right).value_or(std::pair{0, false}).second;
					break;
				default:
					state = false;
					break;
			}

			if (state)
				update_gui_position(controller, predicted_display_period * 1e-9f);
			else
				recentering_context.reset();
		}
		else if (auto state = application::read_action_bool(recenter_left); state and state->second)
			update_gui_position(xr::spaces::aim_left, predicted_display_period * 1e-9f);
		else if (auto state = application::read_action_bool(recenter_right); state and state->second)
			update_gui_position(xr::spaces::aim_right, predicted_display_period * 1e-9f);
		else
			recentering_context.reset();

		std::vector<glm::mat4> world_to_window;
		for (auto & window: imgui_ctx->windows())
		{
			if (window.space == xr::spaces::world)
				world_to_window.push_back(glm::inverse(glm::translate(window.position) * glm::mat4(glm::mat3_cast(window.orientation)) * glm::scale(glm::vec3(window.size, 1))));
		}

		bool hide_left_controller = false;
		bool hide_right_controller = false;

		if (left_hand and right_hand)
		{
			auto left = left_hand->locate(world_space, predicted_display_time);
			auto right = right_hand->locate(world_space, predicted_display_time);

			if (left and xr::hand_tracker::check_flags(*left, XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT, 0))
				hide_left_controller = true;

			if (right and xr::hand_tracker::check_flags(*right, XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT, 0))
				hide_right_controller = true;
		}

		input->apply(world,
		             world_space,
		             predicted_display_time,
		             hide_left_controller,
		             hide_left_controller,
		             hide_right_controller,
		             hide_right_controller,
		             world_to_window);

		// Add the layer with the controllers
		if (composition_layer_depth_test_supported)
		{
			render_world(XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
			             world_space,
			             views,
			             width,
			             height,
			             true,
			             layer_controllers,
			             {});
			set_depth_test(true, XR_COMPARE_OP_ALWAYS_FB);
		}
	}

	for (auto [_, layer]: layers)
	{
		add_quad_layer(layer.layerFlags, layer.space, layer.eyeVisibility, layer.subImage, layer.pose, layer.size);
		if (composition_layer_depth_test_supported)
			set_depth_test(true, XR_COMPARE_OP_LESS_FB);

		else if (alpha < 1 and composition_layer_color_scale_bias_supported)
			set_color_scale_bias({alpha, alpha, alpha, alpha}, {});
	}

	// Display the controller rays
	if (interactable)
	{
		render_world(XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
		             world_space,
		             views,
		             width,
		             height,
		             composition_layer_depth_test_supported,
		             composition_layer_depth_test_supported ? layer_rays : layer_controllers | layer_rays,
		             {});
		if (composition_layer_depth_test_supported)
			set_depth_test(true, XR_COMPARE_OP_LESS_FB);
	}
}
