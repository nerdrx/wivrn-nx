#include "utils/motion_history.h"

#include <cassert>
#include <limits>

using namespace wivrn;

int main()
{
	motion_field_data a{.frame_idx = 1, .span_ns = 10, .width = 1, .height = 1, .scale = 1.f,
	                    .vectors = {10, -10, 20, -20}};
	motion_field_data b = a;
	b.frame_idx = 2;
	b.span_ns = 20;
	b.scale = 2.f;
	b.vectors = {20, -20, 40, -40};
	// Skipped IDs are continuous only with the exact estimator predecessor.
	a.source_time_ns = 1'000'000'000;
	b.source_time_ns = 1'016'666'667;
	b.source_span_ns = 16'666'667;
	b.frame_idx = 4;
	assert(motion_history_contiguous(a, b));
	b.source_time_ns += 16'666'667;
	assert(!motion_history_contiguous(a, b));
	b.source_time_ns = a.source_time_ns;
	assert(!motion_history_contiguous(a, b));
	b.source_time_ns = 0;
	assert(!motion_history_contiguous(a, b));
	b.frame_idx = 2;
	assert(motion_history_contiguous(a, b));
	b.frame_idx = 1;
	assert(!motion_history_contiguous(a, b));
	b.frame_idx = 2;
	motion_field_data out;
	assert(motion_field_ema_blend(a, b, out));
	// Span normalization makes a's 10-unit vector equivalent to 20 units here.
	assert(out.vectors[0] == 15 and out.vectors[1] == -15);
	// A larger previous scale must remain representable after normalization.
	a.scale = 4.f;
	a.vectors = {127, -127, 0, 0};
	b.scale = 1.f;
	b.vectors = {1, -1, 0, 0};
	assert(motion_field_ema_blend(a, b, out));
	assert(out.scale == 4.5f and out.vectors[0] > 110);
	// A past large motion must not permanently coarsen all later small vectors.
	a.span_ns = b.span_ns;
	a.scale = 1.f;
	a.vectors = {127, -127, 0, 0};
	b.scale = .001f;
	b.vectors = {127, -127, 0, 0};
	for (int i = 0; i < 16; ++i)
	{
		assert(motion_field_ema_blend(a, b, out));
		a = out;
	}
	assert(out.scale < .0011f);
	assert(std::abs(float(out.vectors[0]) * out.scale - .127f) < .003f);
	// Invalid metadata never seeds history.
	b.span_ns = 0;
	assert(!motion_field_ema_blend(a, b, out));
	b.span_ns = 20;
	b.scale = std::numeric_limits<float>::quiet_NaN();
	assert(!motion_field_ema_blend(a, b, out));
	// Opposite vectors cancel in normalized-eye space.
	b.scale = 1.f;
	b.vectors = {-127, 127, 0, 0};
	a.scale = b.scale;
	a.span_ns = b.span_ns;
	a.vectors = {127, -127, 0, 0};
	assert(motion_field_ema_blend(a, b, out));
	assert(out.vectors[0] == 0 and out.vectors[1] == 0);
	// Extreme finite scales must not overflow the per-vector arithmetic.
	a.scale = b.scale = std::numeric_limits<float>::max();
	a.vectors = b.vectors = {127, -127, 0, 0};
	assert(motion_field_ema_blend(a, b, out));
	assert(out.vectors[0] == 127 and out.vectors[1] == -127);
	a.span_ns = 1;
	b.span_ns = 100;
	assert(!motion_field_ema_blend(a, b, out));
	a.span_ns = b.span_ns;
	b.vectors.clear();
	assert(!motion_field_ema_blend(a, b, out));
	// Packet validation protects raw warp as well as the optional history path.
	to_headset::motion_field chunk{};
	chunk.width = chunk.height = chunk.row_count = 1;
	chunk.span_ns = 16'666'667;
	chunk.vectors = {0, 0};
	for (float invalid : {-1.f, .251f, std::numeric_limits<float>::infinity(),
	                      std::numeric_limits<float>::quiet_NaN()})
	{
		motion_field_assembler assembler;
		chunk.scale = invalid;
		chunk.view = 0; assembler.add(chunk);
		chunk.view = 1; assembler.add(chunk);
		assert(!assembler.complete());
	}
	chunk.scale = 0;
	for (XrTime invalid : {XrTime(0), XrTime(-1), std::numeric_limits<XrTime>::max()})
	{
		motion_field_assembler assembler;
		chunk.span_ns = invalid;
		chunk.view = 0; assembler.add(chunk);
		chunk.view = 1; assembler.add(chunk);
		assert(!assembler.complete());
	}
	chunk.span_ns = 16'666'667;
	motion_field_assembler stationary;
	chunk.view = 0; stationary.add(chunk);
	chunk.view = 1; stationary.add(chunk);
	assert(stationary.complete());
	// Identity poses pass; a small head turn is rejected.
	assert(motion_pose_delta_small({.orientation = {0, 0, 0, 1}, .position = {0, 0, 0}},
	                               {.orientation = {0, 0, 0, 1}, .position = {0, 0, 0}}));
	assert(!motion_pose_delta_small({.orientation = {0, 0, 0, 1}, .position = {0, 0, 0}},
	                                {.orientation = {0, 0.01f, 0, 0.99995f}, .position = {0, 0, 0}}));
	return 0;
}
