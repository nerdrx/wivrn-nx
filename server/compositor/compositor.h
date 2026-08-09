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
#include "pacer.h"
#include "utils/wivrn_vk_bundle.h"
#include "wivrn_config.h"

#include "main/comp_compositor.h"

#include <array>
#include <atomic>
#include <memory>
#include <thread>

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
	wivrn_session & session;
	vk_bundle vk;
	vk::raii::CommandPool cmd_pool;
	vk::raii::QueryPool query_pool;

	std::array<encoder_settings, 3> settings;

	std::array<image, 2> images;
	vk::raii::CommandBuffer cmd;
	vk::raii::Semaphore sem;
	uint64_t sem_value = 0;

	std::atomic<float> requested_refresh_rate;
	std::atomic<float> frame_rate;
	wivrn::pacer pacer;

	layer_squasher squasher;
	wivrn::foveation foveation;

	// Motion smoothing. The estimator is only built once the headset asks for it,
	// and destroyed again as soon as it stops asking, so a session that never turns
	// the feature on pays nothing at all.
	std::unique_ptr<wivrn::motion_estimator> motion;
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
	// Headset time of the frame the current pyramid was built from
	XrTime motion_previous_display_time = 0;
	// A field was computed into the submission being waited on
	bool motion_pending = false;
	uint64_t motion_frame_index = 0;
	XrTime motion_span = 0;

	std::array<std::unique_ptr<video_encoder>, 3> encoders;

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

	// Detects whether this commit carries a new application frame and, if motion
	// smoothing is wanted and worthwhile, records the estimation work into cmd.
	void update_motion_field(
	        XrTime display_time,
	        uint64_t frame_index,
	        std::array<vk::ImageView, 2> src,
	        std::array<xrt_rect, 2> src_rect,
	        bool flip_y);

	// Sends the field recorded by the last update_motion_field, if any. Must be
	// called once the submission has completed.
	void send_motion_field();

	void encoder_work(std::stop_token);

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

	void set_bitrate(uint32_t);
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
