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

#include "polynomial_interpolator.h"
#include "pose_sanitize.h"
#include "utils/csv_logger.h"
#include "wivrn_packets.h"
#include "xrt/xrt_defines.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

namespace wivrn
{
struct clock_offset;

class pose_list
{
	std::atomic<pose_list *> source = nullptr;
	xrt_pose offset;
	std::atomic_bool derive_forced = false;
	std::mutex mutex;

	polynomial_interpolator<3> positions;
	polynomial_interpolator<4, true> orientations;

public:
	// Whether the standby freeze is applied at all, settable from the headset.
	//
	// One flag for the whole process, on purpose. The pose_list instances are spread
	// over objects that do not know about each other — five in each wivrn_controller,
	// one in view_list for the head, one in wivrn_eye_tracker, more behind the
	// trackers — all built before the first settings packet arrives and none of them
	// reachable from a single owner. The switch is one user-facing checkbox that must
	// apply to every device at once, so there is nothing per instance to say: a
	// process-wide atomic expresses exactly that, and the server hosts one headset
	// session at a time. Read on the tracking and the pose query paths, written only
	// when a settings packet arrives, hence relaxed ordering: a sample either side of
	// the switch is correct either way.
	static void set_standby_freeze(bool enabled)
	{
		standby_freeze_enabled.store(enabled, std::memory_order_relaxed);
	}

	static bool standby_freeze()
	{
		return standby_freeze_enabled.load(std::memory_order_relaxed);
	}

	// Tracking state of one component (position or orientation) of the pose.
	// Some runtimes report poses as valid but not tracked when the device goes to
	// standby, the last known pose must then be kept instead of being extrapolated.
	// Protected by mutex.
	struct tracked_state
	{
		// true if the tracked flag was set at least once, if it never was the runtime
		// probably does not report it at all and the flag is assumed to be set
		bool ever_tracked = false;
		bool currently_tracked = false;
		// timestamp of the last sample that had the tracked flag set
		XrTime last_tracked_timestamp = std::numeric_limits<XrTime>::lowest();
		// production timestamp of the sample that last updated this state,
		// so that out of order samples don't change it back
		XrTime production_timestamp = std::numeric_limits<XrTime>::lowest();
	};

	// Update the tracking state of one component, returns true if the sample must be
	// given to the interpolator. `freeze` is the toggle above, read once by the caller
	// so that both components of a pose see the same value. The state is maintained
	// whatever the toggle says, so that turning it back on freezes at the right sample
	// rather than at whatever happens to arrive next. Must be called with mutex held.
	//
	// Defined here rather than in the translation unit so that the test harness can
	// exercise the policy without linking the driver.
	static bool update_tracked_state(tracked_state & state, XrTime production_timestamp, XrTime timestamp, bool valid, bool tracked, bool freeze)
	{
		// Samples without valid data are forwarded as-is, they invalidate the pose
		if (not valid)
			return true;

		// Ignore out of order samples for the state, they must not undo a newer one
		const bool up_to_date = production_timestamp >= state.production_timestamp;

		if (tracked)
		{
			state.ever_tracked = true;
			if (up_to_date)
			{
				state.currently_tracked = true;
				state.production_timestamp = production_timestamp;
			}
			state.last_tracked_timestamp = std::max(state.last_tracked_timestamp, timestamp);
			return true;
		}

		if (up_to_date)
		{
			state.currently_tracked = false;
			state.production_timestamp = production_timestamp;
		}

		// Runtimes that never report the tracked flag (estimated poses, body joints...)
		// keep the previous behaviour, for the others the sample is dropped so that the
		// interpolation is frozen on the last tracked data.
		// With the freeze turned off every valid sample is fed to the interpolator, which
		// is upstream's behaviour; the state above was still updated, so turning the
		// freeze back on picks up where it left off.
		return not freeze or not state.ever_tracked;
	}

	// Whether a component should be reported as tracked, and its pose queried at the
	// requested time rather than at the last tracked sample.
	static bool component_tracked(const tracked_state & state, bool freeze)
	{
		// Upstream reports every valid pose as tracked. With the freeze on, a component
		// that was never reported as tracked is assumed to always be, else the pose is
		// frozen on the last tracked sample.
		if (not freeze)
			return true;

		return state.currently_tracked or not state.ever_tracked;
	}

private:
	tracked_state position_state;
	tracked_state orientation_state;

	// Samples dropped on ingest because the headset sent a non-finite float.
	// Protected by mutex.
	uint64_t rejected_samples = 0;
	rate_limiter reject_warn;

	struct debug_data
	{
		bool in; // true: received data, false: data request
		XrTime production_timestamp;
		XrTime timestamp;
		XrTime now;
		std::array<float, 3> position;
		std::array<float, 3> dposition;
		std::array<float, 4> orientation;
		std::array<float, 4> dorientation;
		bool position_tracked;
		bool orientation_tracked;
	};

	std::optional<csv_logger<debug_data>> dumper;

public:
	const wivrn::device_id device;

	static xrt_space_relation interpolate(const xrt_space_relation & a, const xrt_space_relation & b, float t);
	static xrt_space_relation extrapolate(const xrt_space_relation & a, const xrt_space_relation & b, int64_t ta, int64_t tb, int64_t t);

	// True if every float the flags claim to be valid is finite and the
	// orientation, if claimed valid, can be normalised.
	static bool is_sane(const from_headset::tracking::pose & pose);

	pose_list(wivrn::device_id id);

	std::pair<XrTime, XrTime> get_bounds() const;

	void update_tracking(const wivrn::from_headset::tracking &, const clock_offset & offset);
	void set_derived(pose_list * source, xrt_pose offset, bool force = false);

	std::tuple<XrTime, xrt_space_relation, device_id> get_pose_at(XrTime at_timestamp_ns);

	void reset();
	std::pair<XrTime, xrt_space_relation> get_at(XrTime at_timestamp_ns);

private:
	void add_sample(XrTime production_timestamp, XrTime timestamp, const from_headset::tracking::pose & pose, const clock_offset & offset);

	// Defaults to the NX behaviour, so that the freeze is already in place for the
	// samples that arrive before the first settings packet is handled.
	static inline std::atomic_bool standby_freeze_enabled{true};
};
} // namespace wivrn
