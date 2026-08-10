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

#pragma once

#include "is_finite.h"
#include "xrt/xrt_defines.h"

#include <cstdint>
#include <limits>
#include <openxr/openxr.h>

namespace wivrn
{

// The scalar is_finite(float)/is_finite(double) primitives (and the reason <cmath>
// classification functions must never be used in this fast-math build) live in
// common/is_finite.h so the client can share them. The overloads below extend them to
// the xrt_ and Xr types.

inline bool is_finite(const xrt_vec3 & v)
{
	return is_finite(v.x) and is_finite(v.y) and is_finite(v.z);
}

inline bool is_finite(const xrt_quat & q)
{
	return is_finite(q.x) and is_finite(q.y) and is_finite(q.z) and is_finite(q.w);
}

inline bool is_finite(const xrt_pose & p)
{
	return is_finite(p.orientation) and is_finite(p.position);
}

inline bool is_finite(const xrt_fov & f)
{
	return is_finite(f.angle_left) and is_finite(f.angle_right) and
	       is_finite(f.angle_up) and is_finite(f.angle_down);
}

inline bool is_finite(const xrt_space_relation & r)
{
	return is_finite(r.pose) and is_finite(r.linear_velocity) and is_finite(r.angular_velocity);
}

// Same checks on the OpenXR types, for validating what the headset sent us before it
// is ever copied into an xrt_ structure.
inline bool is_finite(const XrVector3f & v)
{
	return is_finite(v.x) and is_finite(v.y) and is_finite(v.z);
}

inline bool is_finite(const XrQuaternionf & q)
{
	return is_finite(q.x) and is_finite(q.y) and is_finite(q.z) and is_finite(q.w);
}

inline bool is_finite(const XrPosef & p)
{
	return is_finite(p.orientation) and is_finite(p.position);
}

inline bool is_finite(const XrFovf & f)
{
	return is_finite(f.angleLeft) and is_finite(f.angleRight) and
	       is_finite(f.angleUp) and is_finite(f.angleDown);
}

// True if the quaternion can be used as a rotation: finite, and far enough from zero
// that normalising it is meaningful. A zero quaternion is the usual shape of "the
// runtime never filled this in", and normalising it gives 0/0 = NaN.
inline bool is_valid_orientation(const xrt_quat & q)
{
	if (not is_finite(q))
		return false;

	const float norm2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
	return is_finite(norm2) and norm2 > 0.1f and norm2 < 10.f;
}

inline bool is_valid_orientation(const XrQuaternionf & q)
{
	if (not is_finite(q))
		return false;

	const float norm2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
	return is_finite(norm2) and norm2 > 0.1f and norm2 < 10.f;
}

// Only call on a quaternion that passed is_valid_orientation().
inline void normalize_orientation(xrt_quat & q)
{
	const float norm2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
	const float inv = 1.f / __builtin_sqrtf(norm2);
	q.x *= inv;
	q.y *= inv;
	q.z *= inv;
	q.w *= inv;
}

// True if the relation is safe to hand to the runtime: everything finite, and the
// orientation usable if the flags claim it is valid.
inline bool is_usable(const xrt_space_relation & r)
{
	if (not is_finite(r))
		return false;

	if ((r.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) and
	    not is_valid_orientation(r.pose.orientation))
		return false;

	return true;
}

// Emits at most one event per interval.
class rate_limiter
{
	int64_t interval;
	int64_t next = std::numeric_limits<int64_t>::lowest();

public:
	explicit rate_limiter(int64_t interval_ns = 5'000'000'000) :
	        interval(interval_ns) {}

	bool operator()(int64_t now_ns)
	{
		if (now_ns < next)
			return false;
		next = now_ns + interval;
		return true;
	}
};

// Last line of defence before a pose leaves the driver. The application must never
// see a NaN: xrLocateViews() pushes whatever we return through a relation chain and
// hands it to the game, which submits it back to xrEndFrame() and gets
// XR_ERROR_POSE_INVALID (and, for most engines, a crash).
//
// A relation that is not usable is replaced by the last one that was, with the
// TRACKED and velocity bits cleared: the pose is stale, we must not claim otherwise.
class relation_sanitizer
{
	xrt_space_relation last_good{};
	bool has_last_good = false;
	uint64_t dropped_count = 0;

public:
	uint64_t dropped() const
	{
		return dropped_count;
	}

	// Returns true if the relation was already clean, false if it was replaced.
	bool sanitize(xrt_space_relation & relation)
	{
		if (is_usable(relation))
		{
			last_good = relation;
			has_last_good = true;
			return true;
		}

		++dropped_count;

		// The cache is written and read without a lock: xrt_device entry points are
		// per device and effectively serialised, but re-checking the copy costs
		// nothing and makes a torn read impossible to turn into a bad pose.
		if (has_last_good and is_usable(last_good))
		{
			relation = last_good;
		}
		else
		{
			relation = xrt_space_relation{
			        .relation_flags = XRT_SPACE_RELATION_BITMASK_NONE,
			        .pose = {
			                .orientation = {0, 0, 0, 1},
			                .position = {0, 0, 0},
			        },
			        .linear_velocity = {0, 0, 0},
			        .angular_velocity = {0, 0, 0},
			};
			return false;
		}

		relation.relation_flags = xrt_space_relation_flags(
		        relation.relation_flags &
		        ~(XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
		          XRT_SPACE_RELATION_POSITION_TRACKED_BIT |
		          XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT |
		          XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT));
		relation.linear_velocity = {0, 0, 0};
		relation.angular_velocity = {0, 0, 0};
		return false;
	}
};

// Same idea for the per eye view poses, which are forwarded verbatim from the client
// and are pushed into the relation chain by the runtime with every valid bit set.
class pose_sanitizer
{
	xrt_pose last_good{
	        .orientation = {0, 0, 0, 1},
	        .position = {0, 0, 0},
	};
	uint64_t dropped_count = 0;

public:
	uint64_t dropped() const
	{
		return dropped_count;
	}

	bool sanitize(xrt_pose & pose)
	{
		if (is_finite(pose) and is_valid_orientation(pose.orientation))
		{
			last_good = pose;
			return true;
		}

		++dropped_count;
		pose = is_finite(last_good) and is_valid_orientation(last_good.orientation)
		               ? last_good
		               : xrt_pose{.orientation = {0, 0, 0, 1}, .position = {0, 0, 0}};
		return false;
	}
};

} // namespace wivrn
