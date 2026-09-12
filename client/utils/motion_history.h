#pragma once

#include "motion_field.h"
#include "motion_pose.h"

#include <cmath>
#include <limits>

namespace wivrn
{

// EMA is deliberately conservative: a head turn is already represented by the
// pose compensation path, so filtering it into the object field would mix pose
// mismatch into the normalized-eye motion estimate.
inline bool motion_pose_delta_small(const XrPosef & a, const XrPosef & b)
{
	const float dx = a.position.x - b.position.x;
	const float dy = a.position.y - b.position.y;
	const float dz = a.position.z - b.position.z;
	const float position2 = dx * dx + dy * dy + dz * dz;
	const float dot = std::abs(a.orientation.x * b.orientation.x +
	                           a.orientation.y * b.orientation.y +
	                           a.orientation.z * b.orientation.z +
	                           a.orientation.w * b.orientation.w);
	return position2 <= 1e-8f and dot >= 0.99999f;
}

// Source timestamps identify the estimator's exact predecessor even when video
// IDs skip. Without those timestamps preserve the conservative adjacent-ID rule.
inline bool motion_history_contiguous(const motion_field_data & previous,
                                      const motion_field_data & current)
{
	if (previous.frame_idx == uint64_t(-1) or current.frame_idx <= previous.frame_idx)
		return false;
	if (previous.source_time_ns > 0 and current.source_time_ns > 0)
		return current.source_span_ns > 0 and current.source_span_ns < 100'000'000 and
		       current.source_time_ns > previous.source_time_ns and
		       current.source_time_ns - previous.source_time_ns == current.source_span_ns;
	return current.frame_idx == previous.frame_idx + 1;
}

// Blend fields in normalized-eye units, then quantize to their weighted range.
// This keeps a scale/span change from changing the EMA's physical velocity.
inline bool motion_field_ema_blend(const motion_field_data & previous,
                                   const motion_field_data & current,
                                   motion_field_data & result)
{
	if (current.width == 0 or current.height == 0 or
	    current.vectors.size() != current.value_count() or
	    previous.width != current.width or previous.height != current.height or
	    previous.vectors.size() != current.vectors.size() or current.scale <= 0 or
	    previous.scale <= 0 or previous.span_ns <= 0 or current.span_ns <= 0 or
	    not std::isfinite(previous.scale) or not std::isfinite(current.scale))
		return false;
	const double history_range = double(previous.scale) * double(current.span_ns) / double(previous.span_ns);
	const double range = 0.5 * (double(current.scale) + history_range);
	if (not std::isfinite(range) or range > std::numeric_limits<float>::max() or float(range) <= 0)
		return false;
	// Normalize once. Bounded weights avoid per-vector division and overflow
	// from multiplying a byte by a large but finite input scale.
	const float old_weight = float(0.5 * history_range / float(range));
	const float new_weight = float(0.5 * double(current.scale) / float(range));
	result = current;
	result.scale = float(range);
	for (size_t i = 0; i < current.vectors.size(); ++i)
	{
		const float quantized = std::round(float(previous.vectors[i]) * old_weight +
		                                   float(current.vectors[i]) * new_weight);
		result.vectors[i] = int8_t(std::clamp(quantized, -127.f, 127.f));
	}
	return true;
}

} // namespace wivrn
