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

#include "view_list.h"
#include "os/os_time.h"
#include "pose_sanitize.h"
#include "util/u_logging.h"
#include "xrt_cast.h"

namespace wivrn
{

void view_list::update_tracking(const from_headset::tracking & tracking, const clock_offset & offset)
{
	if (not std::ranges::contains(tracking.device_poses, device_id::HEAD, &from_headset::tracking::pose::device))
		return;

	std::lock_guard lock(mutex);
	flags = tracking.view_flags;

	for (size_t eye = 0; eye < 2; ++eye)
	{
		const auto & view = tracking.views[eye];

		// A runtime that reports the views as invalid usually leaves the array
		// untouched, and the client zeroes it before every locate_views(): the
		// pose then arrives here as a zero quaternion, which the runtime happily
		// multiplies into the view matrix and hands to the application.
		xrt_pose pose = xrt_cast(view.pose);
		if (not pose_sanitizers[eye].sanitize(pose))
		{
			++rejected_views;
			if (reject_warn(os_monotonic_get_ns()))
				U_LOG_W("HEAD: dropped an unusable view pose for eye %zu (%lu so far)",
				        eye,
				        (unsigned long)rejected_views);
		}
		poses[eye] = pose;

		// A non-finite fov turns into a non-finite projection matrix.
		if (is_finite(view.fov))
			fovs[eye] = xrt_cast(view.fov);
	}

	head_poses.update_tracking(tracking, offset);
}

std::pair<XrTime, tracked_views> view_list::get_at(XrTime at_timestamp_ns)
{
	std::lock_guard lock(mutex);
	auto [t, pose] = head_poses.get_at(at_timestamp_ns);
	return {
	        t,
	        tracked_views{
	                .flags = flags,
	                .relation = pose,
	                .poses = poses,
	                .fovs = fovs,
	        },

	};
}
} // namespace wivrn
