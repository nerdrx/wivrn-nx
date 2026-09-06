/*
 * WiVRn VR streaming
 * Copyright (C) 2025  WiVRn contributors
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

#pragma once

// One NX Warp stream's state, as the encoder reports it every two seconds.
//
// This is the same set of numbers video_encoder_nxwarp.cpp writes to the log, which was the only
// place they existed. It travels twice:
//
//   encoder (monado process)  --IPC, binary, boost::pfr-->  wivrn-server main process
//   main process              --D-Bus "NxwarpStats", JSON-->  dashboard
//
// The second hop is JSON rather than a D-Bus struct signature deliberately. A twenty-field struct
// signature has to be spelled identically in the interface XML, in gdbus-codegen's C and in Qt's
// generated C++, and every field added later breaks all three at once; the interface already
// carries JsonConfiguration as a string for the same reason. It stays introspectable -- `busctl
// get-property ... NxwarpStats` prints readable JSON.
//
// Every field is a plain value so the struct serializes over the IPC socket by aggregate
// reflection, with no traits to write.

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace wivrn
{

// Why the headset did not reconstruct a frame. Same order and meaning as the why_name[] table in
// video_encoder_nxwarp.cpp's two-second report.
enum class nxwarp_not_held_reason : uint8_t
{
	hole = 0,           // a datagram never arrived, so the frame had a gap in it
	decode_stride = 1,  // the pacing controller's stride skipped it deliberately
	worker_backlog = 2, // the decoder's workers were still busy with an earlier frame
	codec_refusal = 3,  // the codec itself rejected the unit
	unknown = 4,
};

inline const char * nxwarp_not_held_reason_name(nxwarp_not_held_reason r)
{
	switch (r)
	{
		case nxwarp_not_held_reason::hole:
			return "a hole";
		case nxwarp_not_held_reason::decode_stride:
			return "the decode stride";
		case nxwarp_not_held_reason::worker_backlog:
			return "the worker backlog";
		case nxwarp_not_held_reason::codec_refusal:
			return "a codec refusal";
		case nxwarp_not_held_reason::unknown:
			break;
	}
	return "an unknown cause";
}

// How the encoder decides when to send.
enum class nxwarp_pace_report : uint8_t
{
	automatic = 0, // follow the rate the headset says it can decode at
	off = 1,       // send every composited frame
	fixed = 2,     // hold a configured frame rate exactly
};

struct nxwarp_stream_stats
{
	// Which video stream this is: 0 and 1 are the eyes.
	uint8_t stream_index = 0;

	// --- the reporting window ------------------------------------------------
	// Everything counted below happened in this many seconds. The encoder reports about
	// every two, but the window is the measured one, not the nominal one, so a rate derived
	// from it is right even when a report is late.
	float window_seconds = 0;
	uint64_t frames_encoded = 0;

	// --- encode cost ---------------------------------------------------------
	float encode_ms_mean = 0;
	float encode_ms_max = 0;

	// --- size and quantiser --------------------------------------------------
	float bytes_per_frame = 0;
	// What the rate controller was aiming at. Zero when the quantiser is fixed, in which
	// case there is no target to miss.
	float target_bytes_per_frame = 0;
	float qp_mean = 0;
	uint8_t qp_min = 0;
	uint8_t qp_max = 0;
	bool rc_auto = true;
	// The controller wants a quantiser below its floor or above its ceiling and cannot have
	// one: the configured band cannot reach the bitrate it was given.
	bool rc_unreachable = false;
	// What the session's bitrate controller currently allows this stream. Every other number
	// here is downstream of it.
	uint64_t controller_bitrate_bps = 0;

	// --- pacing --------------------------------------------------------------
	nxwarp_pace_report pace_mode = nxwarp_pace_report::automatic;
	float paced_fps = 0;
	// As the headset measures it. Zero means it has not reported one yet.
	float client_decode_ms = 0;

	// --- where the headset's GPU decode goes ---------------------------------
	//
	// From from_headset::nxwarp_decode_profile, republished about twice a second by
	// the headset's own decoder. `client_decode_ms` above is the wall cost of a
	// decode; these say what it is spent on, which is the difference between "the
	// headset is slow" and "the headset is spending it all warping skipped tiles".
	//
	// `client_pass_b_ms` is an ENVELOPE, not a sum: nxvc measures it from the end of
	// Pass A to the end of Pass B, so it contains the predictor dispatch
	// (`client_pass_w_ms`) and all three reconstruction segments. What is left after
	// subtracting them is the pipeline drain between dispatches -- real time that
	// belongs to no segment. It is displayed as "other" rather than folded in, so the
	// parts visibly account for the whole.
	//
	// All zero, with `client_pass_segments_known` false, when the headset's nxvc has
	// no segment timers or its device cannot timestamp them -- which a reader must be
	// able to tell from a decode that genuinely cost nothing.
	bool client_pass_segments_known = false;
	float client_pass_a_ms = 0;
	float client_pass_b_ms = 0;
	float client_pass_w_ms = 0;
	float client_pass_b_skip_ms = 0;
	float client_pass_b_coded_ms = 0;
	float client_pass_b_dir_ms = 0;
	// Mean tiles per frame in each Pass B segment, eye pass 0. A segment at zero
	// milliseconds with no tiles was empty; with tiles, it was not measurable.
	float client_tiles_skip = 0;
	float client_tiles_coded = 0;
	float client_tiles_dir = 0;

	// The part of the Pass B envelope that no segment timer covers: the drain between
	// dispatches. Never negative -- the envelope and the segments are separate
	// timestamp pairs, so on a very cheap frame the parts can total a hair over.
	float client_pass_b_other_ms() const
	{
		const float parts = client_pass_w_ms + client_pass_b_skip_ms +
		                    client_pass_b_coded_ms + client_pass_b_dir_ms;
		const float other = client_pass_b_ms - parts;
		return other > 0 ? other : 0.f;
	}
	// Composited frames the pacer did not send in this window. Not a fault: it is the
	// difference between what the compositor made and what the headset could take.
	uint64_t frames_not_sent = 0;

	// --- what the headset threw away -----------------------------------------
	uint64_t not_reconstructed = 0;
	// Of those, the ones that cost an all-intra frame. The rest named a frame older than one
	// already coded intra and cost nothing.
	uint64_t not_reconstructed_costly = 0;
	// The reason that accounts for the most of them in this window.
	nxwarp_not_held_reason dominant_reason = nxwarp_not_held_reason::unknown;
	uint64_t dominant_reason_count = 0;

	// --- what was negotiated, once per session -------------------------------
	// The entropy coder actually in use: "rans" or "lite", whatever "entropy" asked for.
	std::string entropy;
	// Whether the configuration asked for it or it was resolved from the headset's tools.
	bool entropy_was_auto = true;
	// The nxvc tool mask the headset advertised.
	uint64_t negotiated_tools = 0;
	// The encoder effort level in use: 0 the plain dead-zone quantiser, 1 the integer
	// requantiser as well.  It is the one negotiated fact that CANNOT be read off the
	// wire -- the level leaves no tool bit, by design, because it changes which levels
	// are coded and nothing about how they decode -- so the encoder reporting it here is
	// the only way anything downstream can say which one produced a stream.
	uint32_t effort = 1;

	// --- the encode size -----------------------------------------------------
	// Per eye, whether or not the eyes are paired: the stereo frame on the wire is twice
	// this wide, which paired_frame_width() below gives.
	uint16_t encoded_width = 0;
	uint16_t encoded_height = 0;
	// Eyes carried by this stream: 2 when both are coded as one nxvc stereo frame on
	// stream 0 (see "stereo-frame"), 1 otherwise. A paired stream 0 means stream 1 has no
	// encoder and will never report -- it is not a stream sitting at zero.
	uint8_t paired_eyes = 1;
	// min(headset render_scale, the server's "stream_scale"): the linear factor the size
	// above was derived at.
	float encode_scale = 1;

	// --- the lens mask -------------------------------------------------------
	//
	// Tiles PER EYE whose whole area falls outside what the headset's optics can show, and
	// which are therefore not worth coding (client/utils/view_geometry.h, and the
	// "lens-mask" option). `lens_mask_tiles` is the denominator, which equals tiles()
	// whenever the codec's grid and the encode size agree -- it is carried separately
	// rather than derived so that a disagreement shows up as two numbers that differ
	// instead of as a share quietly computed against the wrong one.
	//
	// `lens_mask_enforced` is the honest half: TRUE when the codec was actually told
	// (nxvc's skip map, which the reference encoder has), FALSE when the mask is known
	// and reported but the backend has no per-tile mode override, in which case the
	// saving is only what the mode search makes of the flat grey the compositor paints
	// there. Zero masked tiles with the option on is a real answer, not a missing one: a
	// strongly foveated encode has no fully-invisible tile left.
	// Tiles the frames of this window actually put bytes in, and how many they had, both
	// over the CODED FRAME (the eye pair when paired). A tile in WARP_SKIP is not coded.
	// This is the number the lens mask moves, and the pair (coded, total) is carried
	// rather than a ratio so that a window with no frames is visibly empty.
	float tiles_coded_per_frame = 0;
	float tiles_per_frame = 0;

	uint32_t lens_mask_masked = 0;
	uint32_t lens_mask_tiles = 0;
	uint8_t lens_mask_margin = 0;
	bool lens_mask_on = false;
	bool lens_mask_enforced = false;

	// --- how this window's frames were laid on the transport's tile grid -----
	//
	// Reported rather than derived from the configuration, because the choice is made per
	// FRAME and not per session: a frame with any tile too big for a transport slot falls
	// back on its own, so "tile-map": "auto" can produce all spans, all chunks or a mix,
	// and only the encoder knows which. A reader wanting the setting has the
	// configuration; these two are what actually happened.
	uint64_t span_frames = 0;
	uint64_t chunk_frames = 0;

	// --- the latency budget, one stage per field, milliseconds ---------------
	//
	// Mean over the frames of this window that reached the screen, from
	// from_headset::feedback. Every one of these was unavailable for NX Warp until the
	// server started filling timing_info for it: the four server stamps were zero, and
	// `sent_to_decoder` was stamped at publish rather than at hand-off, which reported
	// the decode as taking no time and charged its whole cost to the queue ahead of it.
	//
	// The stages are contiguous and sum to `latency_total_ms`, so a reader can treat
	// them as a stacked bar without normalising:
	//
	//   encode        encode_begin        -> encode_end
	//   wait_send     encode_end          -> send_begin      (pacing, and the queue to it)
	//   send          send_begin          -> send_end        (datagrams handed to the socket)
	//   net           send_begin          -> rx_first        (first byte across the link)
	//   wire          rx_first            -> rx_last         (the frame's span on the wire)
	//   queue         rx_last             -> sent_to_decoder (the bounded worker queue)
	//   decode        sent_to_decoder     -> rx_from_decoder
	//   present       rx_from_decoder     -> blitted
	//
	// `net` overlaps `send` by construction -- both start at send_begin -- because the
	// first byte can arrive before the last has left. It is reported rather than
	// folded in so a reader can see the link's own delay.
	//
	// Zero when the headset has not returned a complete report yet.
	float latency_encode_ms = 0;
	float latency_wait_send_ms = 0;
	float latency_send_ms = 0;
	float latency_net_ms = 0;
	float latency_wire_ms = 0;
	float latency_queue_ms = 0;
	float latency_decode_ms = 0;
	float latency_present_ms = 0;
	float latency_total_ms = 0;
	// Frames the means above are over. Zero means no complete report arrived.
	uint64_t latency_frames = 0;

	// Tiles per eye, the NX Warp decoder's per-frame unit of work.
	uint32_t tiles() const
	{
		return uint32_t(encoded_width / 64) * uint32_t(encoded_height / 64);
	}

	bool paired() const
	{
		return paired_eyes > 1;
	}

	// The frame actually coded: the eyes side by side when paired, one eye otherwise.
	uint32_t coded_frame_width() const
	{
		return uint32_t(encoded_width) * (paired() ? 2u : 1u);
	}

	// Tiles in that frame, which is what one decode dispatch actually costs.
	uint32_t coded_frame_tiles() const
	{
		return tiles() * (paired() ? 2u : 1u);
	}

	float fps_sent() const
	{
		return window_seconds > 0 ? float(double(frames_encoded) / double(window_seconds)) : 0.f;
	}
};

inline void to_json(nlohmann::json & j, const nxwarp_stream_stats & s)
{
	// Floats are rounded to three decimals on the way out. A float widened to double prints as
	// 32.79999923706055, and this property is meant to be readable straight off `busctl
	// get-property`; three decimals is far finer than anything here is measured to.
	auto r = [](float v) {
		return std::round(double(v) * 1000.0) / 1000.0;
	};
	j = nlohmann::json{
	        {"stream_index", s.stream_index},
	        {"window_seconds", r(s.window_seconds)},
	        {"frames_encoded", s.frames_encoded},
	        {"encode_ms_mean", r(s.encode_ms_mean)},
	        {"encode_ms_max", r(s.encode_ms_max)},
	        {"bytes_per_frame", r(s.bytes_per_frame)},
	        {"target_bytes_per_frame", r(s.target_bytes_per_frame)},
	        {"qp_mean", r(s.qp_mean)},
	        {"qp_min", s.qp_min},
	        {"qp_max", s.qp_max},
	        {"rc_auto", s.rc_auto},
	        {"rc_unreachable", s.rc_unreachable},
	        {"controller_bitrate_bps", s.controller_bitrate_bps},
	        {"pace_mode", uint8_t(s.pace_mode)},
	        {"paced_fps", r(s.paced_fps)},
	        {"client_decode_ms", r(s.client_decode_ms)},
	        {"client_pass_segments_known", s.client_pass_segments_known},
	        {"client_pass_a_ms", r(s.client_pass_a_ms)},
	        {"client_pass_b_ms", r(s.client_pass_b_ms)},
	        {"client_pass_w_ms", r(s.client_pass_w_ms)},
	        {"client_pass_b_skip_ms", r(s.client_pass_b_skip_ms)},
	        {"client_pass_b_coded_ms", r(s.client_pass_b_coded_ms)},
	        {"client_pass_b_dir_ms", r(s.client_pass_b_dir_ms)},
	        // Derived, and published anyway: every consumer needs it and none of them
	        // should have to know that the segments do not sum to the envelope.
	        {"client_pass_b_other_ms", r(s.client_pass_b_other_ms())},
	        {"client_tiles_skip", r(s.client_tiles_skip)},
	        {"client_tiles_coded", r(s.client_tiles_coded)},
	        {"client_tiles_dir", r(s.client_tiles_dir)},
	        {"frames_not_sent", s.frames_not_sent},
	        {"not_reconstructed", s.not_reconstructed},
	        {"not_reconstructed_costly", s.not_reconstructed_costly},
	        {"dominant_reason", uint8_t(s.dominant_reason)},
	        {"dominant_reason_count", s.dominant_reason_count},
	        {"effort", s.effort},
	        {"entropy", s.entropy},
	        {"entropy_was_auto", s.entropy_was_auto},
	        {"negotiated_tools", s.negotiated_tools},
	        {"tiles_coded_per_frame", s.tiles_coded_per_frame},
	        {"tiles_per_frame", s.tiles_per_frame},
	        {"lens_mask_masked", s.lens_mask_masked},
	        {"lens_mask_tiles", s.lens_mask_tiles},
	        {"lens_mask_margin", s.lens_mask_margin},
	        {"lens_mask_on", s.lens_mask_on},
	        {"lens_mask_enforced", s.lens_mask_enforced},
	        {"encoded_width", s.encoded_width},
	        {"encoded_height", s.encoded_height},
	        {"paired_eyes", s.paired_eyes},
	        {"encode_scale", r(s.encode_scale)},
	        {"span_frames", s.span_frames},
	        {"chunk_frames", s.chunk_frames},
	        {"latency_encode_ms", r(s.latency_encode_ms)},
	        {"latency_wait_send_ms", r(s.latency_wait_send_ms)},
	        {"latency_send_ms", r(s.latency_send_ms)},
	        {"latency_net_ms", r(s.latency_net_ms)},
	        {"latency_wire_ms", r(s.latency_wire_ms)},
	        {"latency_queue_ms", r(s.latency_queue_ms)},
	        {"latency_decode_ms", r(s.latency_decode_ms)},
	        {"latency_present_ms", r(s.latency_present_ms)},
	        {"latency_total_ms", r(s.latency_total_ms)},
	        {"latency_frames", s.latency_frames},
	};
}

inline void from_json(const nlohmann::json & j, nxwarp_stream_stats & s)
{
	// Every field is optional on the way in: a dashboard talking to an older server must show
	// what it understands rather than refuse the whole payload.
	auto get = [&j](const char * key, auto & out) {
		if (auto it = j.find(key); it != j.end() and not it->is_null())
		{
			try
			{
				out = it->get<std::remove_reference_t<decltype(out)>>();
			}
			catch (...)
			{}
		}
	};
	get("stream_index", s.stream_index);
	get("window_seconds", s.window_seconds);
	get("frames_encoded", s.frames_encoded);
	get("encode_ms_mean", s.encode_ms_mean);
	get("encode_ms_max", s.encode_ms_max);
	get("bytes_per_frame", s.bytes_per_frame);
	get("target_bytes_per_frame", s.target_bytes_per_frame);
	get("qp_mean", s.qp_mean);
	get("qp_min", s.qp_min);
	get("qp_max", s.qp_max);
	get("rc_auto", s.rc_auto);
	get("rc_unreachable", s.rc_unreachable);
	get("controller_bitrate_bps", s.controller_bitrate_bps);
	get("paced_fps", s.paced_fps);
	get("client_decode_ms", s.client_decode_ms);
	get("client_pass_segments_known", s.client_pass_segments_known);
	get("client_pass_a_ms", s.client_pass_a_ms);
	get("client_pass_b_ms", s.client_pass_b_ms);
	get("client_pass_w_ms", s.client_pass_w_ms);
	get("client_pass_b_skip_ms", s.client_pass_b_skip_ms);
	get("client_pass_b_coded_ms", s.client_pass_b_coded_ms);
	get("client_pass_b_dir_ms", s.client_pass_b_dir_ms);
	get("client_tiles_skip", s.client_tiles_skip);
	get("client_tiles_coded", s.client_tiles_coded);
	get("client_tiles_dir", s.client_tiles_dir);
	// client_pass_b_other_ms is deliberately NOT read back: it is derived from the
	// fields above, and a payload whose parts and remainder disagreed would otherwise
	// be reproduced here rather than recomputed.
	get("frames_not_sent", s.frames_not_sent);
	get("not_reconstructed", s.not_reconstructed);
	get("not_reconstructed_costly", s.not_reconstructed_costly);
	get("dominant_reason_count", s.dominant_reason_count);
	get("effort", s.effort);
	get("entropy", s.entropy);
	get("entropy_was_auto", s.entropy_was_auto);
	get("negotiated_tools", s.negotiated_tools);
	get("tiles_coded_per_frame", s.tiles_coded_per_frame);
	get("tiles_per_frame", s.tiles_per_frame);
	get("lens_mask_masked", s.lens_mask_masked);
	get("lens_mask_tiles", s.lens_mask_tiles);
	get("lens_mask_margin", s.lens_mask_margin);
	get("lens_mask_on", s.lens_mask_on);
	get("lens_mask_enforced", s.lens_mask_enforced);
	get("encoded_width", s.encoded_width);
	get("encoded_height", s.encoded_height);
	get("paired_eyes", s.paired_eyes);
	get("encode_scale", s.encode_scale);
	get("span_frames", s.span_frames);
	get("chunk_frames", s.chunk_frames);
	get("latency_encode_ms", s.latency_encode_ms);
	get("latency_wait_send_ms", s.latency_wait_send_ms);
	get("latency_send_ms", s.latency_send_ms);
	get("latency_net_ms", s.latency_net_ms);
	get("latency_wire_ms", s.latency_wire_ms);
	get("latency_queue_ms", s.latency_queue_ms);
	get("latency_decode_ms", s.latency_decode_ms);
	get("latency_present_ms", s.latency_present_ms);
	get("latency_total_ms", s.latency_total_ms);
	get("latency_frames", s.latency_frames);

	uint8_t pace = uint8_t(nxwarp_pace_report::automatic);
	get("pace_mode", pace);
	s.pace_mode = pace <= uint8_t(nxwarp_pace_report::fixed)
	                      ? nxwarp_pace_report(pace)
	                      : nxwarp_pace_report::automatic;

	uint8_t why = uint8_t(nxwarp_not_held_reason::unknown);
	get("dominant_reason", why);
	s.dominant_reason = why <= uint8_t(nxwarp_not_held_reason::unknown)
	                            ? nxwarp_not_held_reason(why)
	                            : nxwarp_not_held_reason::unknown;
}

// Hand one stream's report to the main process, which owns the bus. Implemented in
// server/encoder/nxwarp_stats_publish.cpp; the NX Warp e2e harness links video_encoder_nxwarp.cpp
// without the IPC socket and stubs it. Declared here rather than in wivrn_ipc.h so the encoder
// does not have to pull the socket types in to report a number.
void publish_nxwarp_stats(const nxwarp_stream_stats & stats);

// The whole payload of the NxwarpStats D-Bus property: one entry per active stream, newest report
// per stream. An empty array is the honest answer when nothing is streaming NX Warp.
inline std::string nxwarp_stats_to_json(const std::vector<nxwarp_stream_stats> & streams)
{
	return nlohmann::json(streams).dump();
}

inline std::vector<nxwarp_stream_stats> nxwarp_stats_from_json(const std::string & text)
{
	try
	{
		auto j = nlohmann::json::parse(text);
		if (not j.is_array())
			return {};
		return j.get<std::vector<nxwarp_stream_stats>>();
	}
	catch (...)
	{
		return {};
	}
}

} // namespace wivrn
