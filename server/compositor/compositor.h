/*
 * WiVRn VR streaming
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

// Copyright 2019-2024, Collabora, Ltd.
// Copyright 2025-2026, NVIDIA CORPORATION.

#pragma once

#include "util/comp_base.h"
#include "util/u_logging.h"

#include "encoder/encoder_settings.h"
#include "foveation.h"
#include "layer_squasher.h"
#include "motion_estimator.h"
#include "motion_warper.h"
#include "pacer.h"
#include "quad_converter.h"
#include "utils/wivrn_vk_bundle.h"
#include "wivrn_config.h"

#include "main/comp_compositor.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace wivrn
{

class wivrn_session;
class video_encoder;
class pipewire_mirror;

class compositor : public comp_base
{
public:
	struct image
	{
		std::atomic<bool> busy = false;
		image_allocation image;
		vk::raii::ImageView view_y;
		vk::raii::ImageView view_cbcr;
		to_headset::video_stream_data_shard::view_info_t view_info{};
		// Set when a quad layer was pulled out of this frame's layer stack and
		// converted into the quad stream's own image; the stream is silent on
		// every frame this is empty.
		std::optional<to_headset::video_stream_data_shard::view_info_t::quad_info_t> quad_info;
		uint64_t frame_index;
	};

private:
	struct timings
	{
		std::array<float, 50> values{};
		int index = 0;
		u_var_timing var{
		        .values = {
		                .data = values.data(),
		                .index_ptr = &index,
		                .length = int(values.size()),
		        },
		        .range = 1000,
		        .dynamic_rescale = true,
		        .unit = "µs",
		};

		void add(float us);
	};

	const u_logging_level log_level;
	timings squasher_times;
	timings foveation_times;
	timings motion_times;
	timings motion_warp_times;
	wivrn_session & session;
	vk_bundle vk;
	vk::raii::CommandPool cmd_pool;
	vk::raii::QueryPool query_pool;

	std::array<encoder_settings, num_streams> settings;

	std::array<image, 2> images;
	vk::raii::CommandBuffer cmd;
	vk::raii::Semaphore sem;
	uint64_t sem_value = 0;

	std::atomic<float> requested_refresh_rate;
	std::atomic<float> frame_rate;
	wivrn::pacer pacer;

	layer_squasher squasher;
	wivrn::foveation foveation;

	// Adaptive foveation (foveation v2, lever 2): the curve strength actually pushed to the
	// foveation object, slew-limited toward the target so it does not pop frame to frame. The
	// target is the base setting plus, when adaptive is on and the automatic bitrate is active,
	// a bump proportional to how far the controller has backed off the ceiling. Present thread
	// only. -1 marks "not yet initialised" so the first frame snaps instead of ramping from 0.
	float foveation_adaptive_state = -1;
	std::chrono::steady_clock::time_point foveation_adaptive_last{};
	// Recompute the foveation curve shape for this frame and hand it to the foveation object.
	void update_foveation_shape();

	// Motion smoothing. The estimator is only built once the headset asks for it,
	// and destroyed again as soon as it stops asking, so a session that never turns
	// the feature on pays nothing at all.
	std::unique_ptr<wivrn::motion_estimator> motion;
	// Server-side mode only: the retained frame and the warp that moves it forward.
	// Costs about fifty megabytes at a usual eye size, so it follows the estimator's
	// lifetime and is dropped again the moment the mode or the gating changes.
	std::unique_ptr<wivrn::motion_warper> motion_warp;
	// Fingerprint of the layer stack of the previous commit. The multi client
	// compositor replays a client's layers at display rate until it submits a new
	// frame, so a change here, and nothing else, marks a new application frame.
	uint64_t last_layer_fingerprint = 0;
	// Low pass filtered fraction of commits that carry a new application frame,
	// which is the application frame rate over the stream frame rate
	float app_frame_ratio = 1;
	// Hysteresis on that ratio: whether the application is far enough behind for
	// smoothing to be worth its cost
	bool app_behind = false;
	// The estimator could not be created. Sticky for the session: without it the
	// filtered ratio climbs back over the enter threshold within a fraction of a
	// second and the failing creation is retried several times per second.
	bool motion_failed = false;
	// The warper could not be created. Sticky for the session, same reasoning as
	// motion_failed, except that the fallback is the headset-side mode rather than
	// nothing at all.
	bool motion_warp_failed = false;
	// A submission that may still hold estimator work timed out. Nothing may be
	// recorded into the estimator, and above all it may not be destroyed, until a
	// later wait succeeds.
	bool motion_unsafe = false;
	// Headset time of the frame the current pyramid was built from
	XrTime motion_previous_display_time = 0;
	// A field was computed into the submission being waited on
	bool motion_pending = false;
	uint64_t motion_frame_index = 0;
	XrTime motion_span = 0;

	// What this commit is doing about motion smoothing, decided at the top of
	// layer_commit by motion_begin() because the warp has to be recorded before the
	// foveation pass that reads its output. off covers a headset that asked for
	// nothing, an application that is keeping up, and a submission that timed out.
	motion_mode motion_mode_now = motion_mode::off;
	// Whether this commit carries a new application frame or replays the last one
	bool motion_new_frame = false;
	// Server-side mode: the warper holds a real frame, and what it was rendered for.
	// The pose and fov travel with it because a warped commit has to describe the
	// frame it actually carries, not the one the application would have drawn.
	bool motion_retained = false;
	XrTime motion_retained_display_time = 0;
	std::array<XrPosef, 2> motion_retained_pose{};
	std::array<XrFovf, 2> motion_retained_fov{};
	std::array<xrt_fov, 2> motion_retained_src_fov{};
	// The previous real frame's pose/fov: the far end of the interval the motion
	// field spans (previous real frame -> the retained one). A warped duplicate
	// advances the submitted pose along this segment, the same way the warp advances
	// the picture along the field, so the runtime's timewarp completes the head
	// motion the warp baked in instead of applying it a second time. Cleared when
	// there is no previous real frame yet, in which case the warp falls back to the
	// frozen retained pose.
	bool motion_retained_prev = false;
	std::array<XrPosef, 2> motion_retained_prev_pose{};
	std::array<XrFovf, 2> motion_retained_prev_fov{};
	// Swapchain the quad layer promoted out of the retained frame came from, or null.
	// A commit that would promote a different one must not be warped: the retained
	// image was composited around a different set of layers.
	const void * motion_retained_quad = nullptr;
	// A field starting at the retained frame is in the estimator's buffer, and the
	// interval it spans
	bool motion_retained_field = false;
	XrTime motion_retained_span = 0;
	// Server-side warping is armed, for the headset's Transport page. Written on the
	// present path, read from the thread that assembles the status packet.
	std::atomic<bool> motion_server_warping = false;

	// Read from three threads (the one that presents, the one that encodes, and the
	// network thread that pushes settings down), and written when an encoder is
	// failed over. Shared rather than unique so that a reader that took its copy
	// before a swap can finish the call it is in — including a call wedged inside a
	// driver — on an object that cannot be pulled out from under it.
	mutable std::mutex encoders_mutex;
	std::array<std::shared_ptr<video_encoder>, num_streams> encoders;

	// Encoders that were failed over. Deliberately kept alive, and never destroyed
	// before the compositor is: the reason one is here is that its driver misbehaved
	// or stopped answering, and tearing down a wedged encode session (waiting on its
	// fences, destroying command buffers a submission may still be using) is exactly
	// how a recoverable stall turns into a hung process.
	std::vector<std::shared_ptr<video_encoder>> retired_encoders;

	// Hardware encoder failover: both switches, ANDed by wivrn_session, mirrored
	// here so that set_encoder_failover can tell a real change from the repeated
	// calls a settings_changed packet produces. Atomic because unlike the other
	// mirrors it is read on the present path, once per frame.
	std::atomic<bool> failover_enabled = true;
	// Emergency half-rate mode: the extra divider applied to the stream framerate below
	// the bitrate floor. 1 normally, 2 while engaged. Kept separate from frame_rate so the
	// panel refresh rate reported to the application (frame_rate * fps_divider) never
	// changes: only the encode/pacer rate is halved, exactly as the manual half-rate does.
	// Mutated only from the network thread, same as set_framerate.
	std::atomic<uint32_t> emergency_divider = 1;
	// A stream is running on the software encoder. Sticky for the session: the
	// hardware is not trusted again before a reconnect. Read by the session (and
	// whatever displays state in the headset), and exported to the debug GUI.
	std::atomic<bool> software_fallback = false;
	bool software_fallback_var = false;
	// Same thing per stream, one bit per index, for the headset's Transport page: which
	// eye lost its hardware encoder is the part a user can act on.
	std::atomic<uint8_t> software_fallback_mask = 0;

	// Packet pacing, mirrored here so that set_pacing can tell a real change from the
	// repeated calls a settings_changed packet produces, and so that the transport status
	// packet can report it. Atomic for the latter: the setters run on the network thread
	// and the status is assembled on the worker thread.
	std::atomic<bool> pacing_enabled = false;
	std::atomic<float> pacing_window = 0;
	// Same story for forward error correction, the adaptive parity ratio that extends
	// it, and the shard retransmission that shares its loss measurement
	std::atomic<bool> fec_enabled = false;
	std::atomic<bool> fec_adaptive_enabled = false;
	std::atomic<bool> retransmit_enabled = false;
	// And for intra refresh loss recovery. Starts true because that is the default at
	// both ends: the first call from the session is what makes it real, and it must be
	// able to tell "already on" from a change.
	std::atomic<bool> intra_refresh_enabled = true;
	// And for the rung below it, reference invalidation
	std::atomic<bool> ref_invalidation_enabled = true;

	// Separate streaming of one overlay quad layer. Null unless the headset asked
	// for it when the session was set up, in which case the layer is picked afresh
	// on every commit and the stream stays silent on the commits that pick none.
	std::unique_ptr<wivrn::quad_converter> quad;
	// Read once: constructing a configuration reads the file from disk.
	bool quad_allow_blended = false;
	// Swapchain the promoted layer came from on the previous commit. Only used to
	// keep the choice from flapping between two layers of similar size.
	const void * quad_last_swapchain = nullptr;

#if WIVRN_USE_PIPEWIRE
	// Desktop mirror, null unless enabled in the configuration
	std::unique_ptr<pipewire_mirror> mirror;
#endif

#ifdef __cpp_lib_atomic_lock_free_type_aliases
	using status_type = std::atomic_signed_lock_free;
#else
	using status_type = std::atomic_int8_t;
#endif
	status_type encode_request{-1}; // id of the image to encode
	std::jthread encoder_thread;

	struct
	{
		comp_frame waited{.id = -1};
		comp_frame rendering{.id = -1};
	} frame;

	xrt_result_t begin_session(const xrt_begin_session_info * info)
	{
		return XRT_SUCCESS;
	};
	xrt_result_t end_session()
	{
		return XRT_SUCCESS;
	};

	xrt_result_t predict_frame(int64_t * out_frame_id,
	                           int64_t * out_wake_time_ns,
	                           int64_t * out_predicted_gpu_time_ns,
	                           int64_t * out_predicted_display_time_ns,
	                           int64_t * out_predicted_display_period_ns);

	xrt_result_t mark_frame(int64_t frame_id,
	                        xrt_compositor_frame_point point,
	                        int64_t when_ns);

	xrt_result_t begin_frame(int64_t frame_id)
	{
		return XRT_SUCCESS;
	}
	xrt_result_t discard_frame(int64_t frame_id)
	{
		return XRT_SUCCESS;
	};

	xrt_result_t layer_commit(xrt_graphics_sync_handle_t sync_handle);

	xrt_result_t get_display_refresh_rate(float * out_display_refresh_rate_hz);

public:
	xrt_result_t request_display_refresh_rate(float display_refresh_rate_hz);

	static xrt_result_t get_view_config(xrt_compositor_native *,
	                                    xrt_view_type view_type,
	                                    xrt_view_config * out_view_config);

private:
	void destroy()
	{
		// do nothing, actually owned by wivrn_session object
	}

	int acquire_image();

	// One quad layer of the current layer stack, chosen to be streamed on its own
	// instead of being composited into the eye images.
	struct promoted_quad
	{
		// Index in layer_accum, the layer the squasher must leave out
		int layer_index;
		vk::ImageView view;
		xrt_normalized_rect src_rect;
		uint32_t src_width;
		uint32_t src_height;
		// Width over height of the quad in meters
		float aspect;
		const void * swapchain;
		to_headset::video_stream_data_shard::view_info_t::quad_info_t info;
	};

	// Picks the layer to promote for this commit, or nothing at all, in which case
	// everything behaves exactly as it did before the feature existed.
	std::optional<promoted_quad> select_quad_layer(int64_t display_time_ns);

	// Detects whether this commit carries a new application frame, and decides what
	// motion smoothing does about it: which mode is in force, and whether the
	// estimator and the warper should exist at all. Runs before anything else on the
	// commit reads either of them.
	void motion_begin();

	// Frees the retained frame and the images that hold it
	void drop_retained_frame();

	// Records the estimation work into cmd, on the commits that carry a new
	// application frame and while a mode is in force.
	void update_motion_field(
	        XrTime display_time,
	        uint64_t frame_index,
	        std::array<vk::ImageView, 2> src,
	        std::array<xrt_rect, 2> src_rect,
	        bool flip_y);

	// Server-side mode: keeps this commit's composited views if it carries a new
	// application frame, or warps the retained ones forward if it does not. On a
	// warped commit src, src_rect, src_fov, flip_y and view_info are rewritten to
	// describe the frame that is actually going to be encoded; otherwise nothing
	// downstream can tell this was called.
	void motion_warp_commit(
	        std::array<vk::ImageView, 2> & src,
	        std::array<xrt_rect, 2> & src_rect,
	        std::array<xrt_fov, 2> & src_fov,
	        bool & flip_y,
	        to_headset::video_stream_data_shard::view_info_t & view_info,
	        const void * promoted_swapchain);

	// Sends the field recorded by the last update_motion_field, if any. Must be
	// called once the submission has completed.
	void send_motion_field();

	void encoder_work(std::stop_token);

	// Copy of the encoder slots, taken once per use so that a swap can never land
	// in the middle of a loop over them.
	std::array<std::shared_ptr<video_encoder>, num_streams> get_encoders() const;

	// Ask every encoder's watchdog whether it has given up, and act on it. Called
	// from the present path: the encoder thread is the one that may be stuck in the
	// driver, so it cannot be the one to notice.
	void check_encoder_health();

	// Replace stream `idx`'s encoder with a software one carrying the same live
	// state. Returns whether it worked; logs either way.
	bool fail_over_encoder(size_t idx, const std::string & reason);

	void send_video_stream_description();

public:
	using base_t = xrt_compositor;
	compositor(wivrn_session &);
	~compositor();

	xrt_system_compositor_info sys_info() const;

	int64_t get_frame_duration() const
	{
		return pacer.get_frame_duration();
	}

	float get_requested_refresh_rate() const
	{
		return requested_refresh_rate;
	}

	float get_framerate() const
	{
		return frame_rate;
	}
	void set_framerate(float hz);

	// Emergency half-rate mode (the rung below the bitrate floor): halve the stream
	// framerate to instantly halve bandwidth, and restore it. Live and idempotent, like
	// set_framerate; does not touch resolution and needs no reconnect. Driven by the
	// bitrate controller through wivrn_session. Network thread only.
	void set_emergency_framerate(bool active);
	bool emergency_framerate() const
	{
		return emergency_divider.load() != 1;
	}

	void set_bitrate(uint32_t);

	// Packet pacing: spread each frame's shards over `window` of a frame period
	// instead of handing them to the socket in one burst. Logs state changes.
	void set_pacing(bool enabled, float window);

	// Forward error correction: add a parity shard per group of video shards so
	// that the headset rebuilds a lost one. Logs state changes.
	void set_fec(bool enabled);

	// Let that parity ratio follow the measured loss instead of being the fixed 8+1,
	// and interleave the groups so a burst of consecutive datagrams costs one erasure
	// in several groups rather than several in one. Logs state changes.
	void set_fec_adaptive(bool enabled);

	// Keep a short history of the shards each stream sends, so that one the headset
	// says it never received can be sent again. Logs state changes; off releases the
	// memory.
	void set_shard_retransmit(bool enabled);

	// Rebuild the shards a headset request asks for. Appended to `out` for the session
	// to put on the wire: the connection is the session's, not the compositor's.
	void collect_retransmits(const from_headset::nack &,
	                         std::vector<to_headset::video_stream_data_shard> & out);

	// One NX Warp feedback packet, to the encoder of the stream it names. Only
	// video_encoder_nxwarp does anything with it.
	void on_nxwarp_feedback(const from_headset::nxwarp_feedback &);

	// Video shards sent again on request over every stream, monotonic
	uint64_t retransmitted_shards() const;

	// Hardware encoder failover: hand a stream whose hardware encoder died or went
	// quiet to the software encoder rather than letting it freeze. Logs state
	// changes; turning it off never undoes a swap that already happened.
	void set_encoder_failover(bool enabled);

	// Intra refresh loss recovery: repair a lost frame with a rolling sweep of intra
	// coded blocks rather than with a keyframe. Logs state changes. Only ever narrows
	// live — the mechanism itself is configured when an encoder is created, so turning
	// it back on only takes effect on the next connection.
	void set_intra_refresh(bool enabled);

	// Reference frame invalidation: repair a lost frame by telling the encoder to stop
	// predicting from it, so the next P frame references an older acknowledged one. The
	// cheapest rung of the recovery ladder, tried before the sweep above. Same live
	// semantics: only ever narrows, because the DPB it needs is configured when an
	// encoder is created.
	void set_ref_invalidation(bool enabled);

	// Whether any stream is running on the software encoder after a failover
	bool on_software_encoder() const
	{
		return software_fallback;
	}

	// The same, one bit per stream index. Sticky for the session, like the failover.
	uint8_t software_encoders() const
	{
		return software_fallback_mask;
	}

	// Live state of the two per-frame transport switches, for the headset's Transport
	// page. Both are what the encoders were last told, which is the headset toggle ANDed
	// with the server's own configuration.
	bool fec_active() const
	{
		return fec_enabled;
	}
	bool pacing_active() const
	{
		return pacing_enabled and pacing_window > 0;
	}

	// Whether motion smoothing is in server mode and armed, so that the frames the
	// application did not produce are being synthesized here and encoded
	bool motion_server_active() const
	{
		return motion_server_warping;
	}

	void update_tracking(const from_headset::tracking &);
	void update_foveation_center_override(const from_headset::override_foveation_center &);

	void resume();

	// Force an IDR on every stream, without re-sending the stream description.
	// Used when the path carrying video changes: whatever was in flight on the
	// old path is lost, and a P-frame referencing it would be undecodable.
	void request_idr();

	void on_feedback(const from_headset::feedback &, const clock_offset &);
};

} // namespace wivrn
