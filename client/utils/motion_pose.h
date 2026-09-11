#pragma once

#include "is_finite.h"

#include <algorithm>
#include <openxr/openxr.h>

namespace wivrn
{

inline XrQuaternionf motion_pose_slerp(const XrQuaternionf & a, XrQuaternionf b, float u)
{
	float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
	if (dot < 0)
	{
		b = {-b.x, -b.y, -b.z, -b.w};
		dot = -dot;
	}
	dot = std::clamp(dot, -1.f, 1.f);
	XrQuaternionf r;
	if (dot > 0.9995f)
		r = {a.x + u * (b.x - a.x), a.y + u * (b.y - a.y), a.z + u * (b.z - a.z), a.w + u * (b.w - a.w)};
	else
	{
		const float theta = __builtin_acosf(dot);
		const float sin_theta = __builtin_sinf(theta);
		const float sa = __builtin_sinf((1.f - u) * theta) / sin_theta;
		const float sb = __builtin_sinf(u * theta) / sin_theta;
		r = {sa * a.x + sb * b.x, sa * a.y + sb * b.y, sa * a.z + sb * b.z, sa * a.w + sb * b.w};
	}
	const float n2 = r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w;
	if (n2 > 0)
	{
		const float inv = 1.f / __builtin_sqrtf(n2);
		r = {r.x * inv, r.y * inv, r.z * inv, r.w * inv};
	}
	return r;
}

inline XrPosef motion_pose_extrapolate(const XrPosef & prev, const XrPosef & cur, float u)
{
	return {
	        .orientation = motion_pose_slerp(prev.orientation, cur.orientation, u),
	        .position = {prev.position.x + u * (cur.position.x - prev.position.x),
	                     prev.position.y + u * (cur.position.y - prev.position.y),
	                     prev.position.z + u * (cur.position.z - prev.position.z)},
	};
}

inline bool motion_pose_valid(const XrPosef & p)
{
	const auto & q = p.orientation;
	const float n2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
	return is_finite(q.x) and is_finite(q.y) and is_finite(q.z) and is_finite(q.w) and
	       is_finite(p.position.x) and is_finite(p.position.y) and is_finite(p.position.z) and
	       n2 > 0.999f and n2 < 1.001f;
}

inline bool motion_fov_valid(const XrFovf & f)
{
	return is_finite(f.angleLeft) and is_finite(f.angleRight) and is_finite(f.angleUp) and is_finite(f.angleDown) and
	       f.angleLeft < f.angleRight and f.angleDown < f.angleUp and
	       f.angleLeft > -1.5707963f and f.angleRight < 1.5707963f and
	       f.angleDown > -1.5707963f and f.angleUp < 1.5707963f;
}

} // namespace wivrn
