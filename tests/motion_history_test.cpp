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
	assert(out.scale >= 8.f and out.vectors[0] > 60);
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
	// Identity poses pass; a small head turn is rejected.
	assert(motion_pose_delta_small({.orientation = {0, 0, 0, 1}, .position = {0, 0, 0}},
	                               {.orientation = {0, 0, 0, 1}, .position = {0, 0, 0}}));
	assert(!motion_pose_delta_small({.orientation = {0, 0, 0, 1}, .position = {0, 0, 0}},
	                                {.orientation = {0, 0.01f, 0, 0.99995f}, .position = {0, 0, 0}}));
	return 0;
}
