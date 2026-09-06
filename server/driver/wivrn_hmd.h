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

#pragma once

#include "utils/thread_safe.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_tracking.h"

#include <inplace_vector.hpp>

#include "view_list.h"

#include <array>
#include <cstdint>

namespace wivrn
{
class wivrn_session;

struct wivrn_hmd_presence_data
{
	bool value;
	int64_t change_time;
};

class wivrn_hmd : public xrt_device
{
	beman::inplace_vector::inplace_vector<xrt_input, 2> inputs_array;
	xrt_hmd_parts hmd_parts{};
	xrt_tracking_origin tracking_origin{
	        .name = "WiVRn origin",
	        .type = XRT_TRACKING_TYPE_OTHER,
	        .initial_offset = {
	                .orientation = {0, 0, 0, 1},
	        },
	};

	view_list views;

	// Last line of defence: whatever happens upstream, the application must never
	// be handed a NaN. get_tracked_pose() and get_view_poses() are called from the
	// application's frame thread, one at a time per xrt_device, but they are
	// distinct entry points so each keeps its own cache.
	relation_sanitizer head_relation_sanitizer;
	relation_sanitizer view_relation_sanitizer;
	std::array<pose_sanitizer, 2> view_pose_sanitizers;
	uint64_t sanitized_poses = 0;
	rate_limiter sanitize_warn;

	thread_safe<from_headset::battery> battery{};

	thread_safe<wivrn_hmd_presence_data> presence;
	thread_safe<std::array<std::optional<from_headset::visibility_mask_changed::masks>, 2>> visibility_mask;

	wivrn::wivrn_session * cnx;

	xrt_result_t get_visibility_mask(xrt_visibility_mask_type, uint32_t view_index, xrt_visibility_mask **);

	// Counts and rate limits the "we had to replace a pose" warnings.
	void warn_sanitized(int64_t now, const char * what);

	// Edge bleed overscan, as a fraction of each side's tangent; 0 is off. Read from the
	// configuration once, at construction, like every other driver-level setting here.
	float overscan = 0;

	xrt_fov apply_overscan(const xrt_fov &) const;

public:
	using base_t = xrt_device;
	wivrn_hmd(wivrn::wivrn_session * cnx,
	          const from_headset::headset_info_packet & info);

	void set_foveated_size(uint32_t width, uint32_t height);

	xrt_result_t update_inputs();
	xrt_result_t get_tracked_pose(xrt_input_name name, int64_t at_timestamp_ns, xrt_space_relation *);
	xrt_result_t get_view_poses(const xrt_vec3 * default_eye_relation,
	                            int64_t at_timestamp_ns,
	                            xrt_view_type view_type,
	                            uint32_t view_count,
	                            xrt_space_relation * out_head_relation,
	                            xrt_fov * out_fovs,
	                            xrt_pose * out_poses);
	xrt_result_t get_battery_status(bool * out_present,
	                                bool * out_charging,
	                                float * out_charge);

	void update_battery(const from_headset::battery &);
	void update_tracking(const from_headset::tracking &, const clock_offset &);
	void update_visibility_mask(const from_headset::visibility_mask_changed &);
	void update_presence(bool new_presence, int64_t timestamp);
};
} // namespace wivrn
