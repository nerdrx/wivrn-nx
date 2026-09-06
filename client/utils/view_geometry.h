/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn contributors
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

// View geometry: where the headset's display field of view lands inside a streamed eye
// image, and how much image there is around it.
//
// This is deliberately ONE place, because two independent features need the same numbers
// and disagreeing by a pixel is a visible artefact in both:
//
//   * Edge bleed (overscan). The server renders and encodes a FOV wider than the headset
//     displays. The extra ring is real pixels, so when the headset's compositor reprojects
//     a late frame to a newer pose it has content to move into instead of black. The
//     encoded size does not change, so the margin is paid for in effective resolution.
//
//   * The lens mask. Tiles that fall entirely outside the visible lens region are skipped.
//     The overscan margin is NOT outside the visible region for that purpose -- it exists
//     precisely to be pulled into view by a late reprojection, so masking it away would
//     undo the feature it was added for. Anything that asks "is this part of the image
//     ever seen" must ask through `is_maskable()` below, which answers no for the margin.
//
// Header only, no Vulkan, no Qt, no OpenXR: the server (server/driver/wivrn_hmd.cpp) and
// the client (client/scenes/stream_defoveator.cpp) both include it, and
// tests/view_geometry_test.cpp checks the round trips. Angles are the signed half angles
// both XrFovf and xrt_fov use -- left and down negative, right and up positive -- passed
// as bare floats so neither type has to be visible here.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace wivrn::view_geometry
{

// The margin is a fraction of the projection plane's half extent on the side it is
// applied to: tan(angle) is scaled by (1 + fraction). "5 % per side" therefore adds 5 %
// more picture beyond each edge, and grows the whole plane by 5 %, of which the new ring
// is f / (1 + f) -- about 4.8 % of the widened image, or 2.4 % of its width per side.
//
// It is expressed on the plane rather than in degrees because that is the quantity the
// pixels are uniform in: a projection layer is a flat image, so 5 % of the plane is 5 %
// more pixels on that side whatever the FOV happens to be, while 5 degrees is a wildly
// different number of pixels at the centre and at the edge of a wide FOV. `to_degrees()`
// below converts for display; the wire and the configuration carry the fraction.
inline constexpr float default_overscan = 0.05f;

// A margin above this is not a bleed any more, it is a different stream: at 0.5 a third
// of every encoded pixel is spent outside the panel. Clamped rather than refused so a
// hand-edited configuration degrades instead of failing to start.
inline constexpr float max_overscan = 0.5f;

// Sanitised margin. A NaN, an infinity or a negative all mean "off": the value reaches
// here from a JSON file and from the wire, and neither is trusted.
inline constexpr float clamp_overscan(float fraction)
{
	if (not(fraction > 0))
		return 0.f; // also catches NaN
	return std::min(fraction, max_overscan);
}

// One edge of a projection FOV, widened by `fraction`. The sign convention does the work:
// tan() is negative on the left and bottom edges, so scaling it by (1 + fraction) always
// moves the edge outward.
//
// An angle at or beyond +/- 90 degrees has no finite tangent and cannot be widened on a
// projection layer at all; it is returned untouched rather than turned into a NaN that
// would reach xrEndFrame as XR_ERROR_POSE_INVALID.
inline float overscan_angle(float angle, float fraction)
{
	const float f = clamp_overscan(fraction);
	if (f == 0.f or not std::isfinite(angle))
		return angle;

	const float t = std::tan(angle);
	if (not std::isfinite(t))
		return angle;

	const float widened = std::atan(t * (1.f + f));
	return std::isfinite(widened) ? widened : angle;
}

// How many degrees `fraction` adds to this particular edge. Only for showing a number to
// a person -- the HUD line and the dashboard's help text -- since it depends on the edge.
inline float to_degrees(float angle, float fraction)
{
	constexpr float rad_to_deg = 57.29577951308232f;
	return (overscan_angle(angle, fraction) - angle) * rad_to_deg;
}

// Normalised sub-rectangle, 0..1 with the origin at the top left of the image, the
// orientation the eye image and its texture coordinates are in.
struct rect
{
	float x0 = 0.f, y0 = 0.f, x1 = 1.f, y1 = 1.f;

	constexpr float width() const
	{
		return x1 - x0;
	}
	constexpr float height() const
	{
		return y1 - y0;
	}

	bool operator==(const rect &) const = default;
};

// The part of an overscanned eye image the headset actually displays, given the DISPLAY
// FOV (the un-widened one, as the headset reported it) and the margin the server applied.
//
// The image spans tan(left') .. tan(right') where the primed angles are the widened ones,
// so the display's tan(left) .. tan(right) lands at
//
//     x0 = (tan(left) - tan(left')) / (tan(right') - tan(left'))
//        = -f tan(left) / ((1 + f) (tan(right) - tan(left)))
//
// and symmetrically on the other three sides. The tangents cancel out of nothing here:
// an asymmetric FOV, which every real headset has, gets an asymmetric margin in pixels
// even though every side grew by the same fraction, and that asymmetry is the whole
// reason this is not a single inset.
//
// y is flipped because the image's first row is the TOP of the view, which is the `up`
// edge, while `up` is the positive angle.
inline rect visible_rect(float left, float right, float down, float up, float fraction)
{
	const float f = clamp_overscan(fraction);
	if (f == 0.f)
		return {};

	const float tl = std::tan(left), tr = std::tan(right);
	const float td = std::tan(down), tu = std::tan(up);
	const float w = tr - tl, h = tu - td;

	// A degenerate or non-finite FOV cannot be divided by. Report the whole image as
	// visible, which is what every consumer here treats as "no margin, mask nothing".
	if (not(std::isfinite(w) and std::isfinite(h) and w > 0.f and h > 0.f))
		return {};

	const float k = f / (1.f + f);
	const float x0 = k * (-tl) / w;
	const float x1 = 1.f - k * tr / w;
	const float y0 = k * tu / h; // top of the image is the `up` edge
	const float y1 = 1.f - k * (-td) / h;

	return {std::clamp(x0, 0.f, 1.f),
	        std::clamp(y0, 0.f, 1.f),
	        std::clamp(x1, 0.f, 1.f),
	        std::clamp(y1, 0.f, 1.f)};
}

// The same rectangle in the -1..1 clip space `inPosition` carries in the reprojection
// shader, where +y is DOWN the image (Vulkan's clip space), so the flip above is already
// baked in and this is a plain remap.
inline rect visible_ndc(float left, float right, float down, float up, float fraction)
{
	const rect r = visible_rect(left, right, down, up, fraction);
	return {r.x0 * 2.f - 1.f, r.y0 * 2.f - 1.f, r.x1 * 2.f - 1.f, r.y1 * 2.f - 1.f};
}

// Shorthand for the symmetric case, which is what a test or a sanity check wants: every
// side is inset by f / (2 (1 + f)) of the image. Real headsets are not symmetric; this is
// not a substitute for visible_rect(), it is the number to check visible_rect() against.
inline constexpr float symmetric_inset(float fraction)
{
	const float f = clamp_overscan(fraction);
	return f / (2.f * (1.f + f));
}

// May a consumer that only cares about what the panel shows -- the lens mask -- throw this
// part of the image away?
//
// No, whenever any of it lies inside the overscan margin. The margin is off the panel
// *right now*, at the pose the frame was rendered for, and it is on the panel a few
// milliseconds later at the pose the frame is displayed at: that is what it is for. A mask
// that treats "outside the visible rect" as "never seen" would delete exactly the pixels
// the bleed exists to provide, and the black edge would come back with the overscan
// setting still reading 5 %.
//
// `r` is the region under test, in the same normalised image coordinates as
// visible_rect(). The answer is a plain intersection test against the *visible* rect: a
// region is maskable only when it is disjoint from it.
inline bool is_maskable(const rect & r, const rect & visible)
{
	return r.x1 <= visible.x0 or r.x0 >= visible.x1 or
	       r.y1 <= visible.y0 or r.y0 >= visible.y1;
}

// How the reprojection pass fills a sample that falls outside the decoded image. This is
// the guarantee that survives overscan being off: the pass must never emit the clear
// colour, whatever the pose delta was.
enum class edge_extension : uint8_t
{
	// Sample as-is. Outside the image is whatever the sampler's addressing mode gives,
	// which on the border-colour path is black. Only for measuring the artefact.
	none = 0,
	// Clamp the sample to the edge texel: the outermost row and column are stretched
	// outward for as long as the sample stays outside. Cheap, exact at one pixel out,
	// and increasingly obviously a smear the further out it goes.
	clamp = 1,
	// Clamp, and past `fade_distance` blend toward the edge's own averaged colour so a
	// long stretch decays into a matching wash instead of a streak. The default.
	fade = 2,
};

inline constexpr edge_extension default_edge_extension = edge_extension::fade;

// Distance, as a fraction of the eye image, over which `fade` moves from the stretched
// edge texel to the averaged edge colour. Small: past a few percent of the image the
// stretch has no information left in it.
inline constexpr float default_fade_distance = 0.02f;
inline constexpr float max_fade_distance = 0.25f;

inline constexpr float clamp_fade_distance(float d)
{
	if (not(d > 0))
		return 0.f;
	return std::min(d, max_fade_distance);
}

inline constexpr const char * name(edge_extension e)
{
	switch (e)
	{
		case edge_extension::none:
			return "none";
		case edge_extension::clamp:
			return "clamp";
		case edge_extension::fade:
			return "fade";
	}
	return "fade";
}

// Parse the spelling the configuration and the dashboard use. An unknown string is the
// default rather than an error: this arrives from a JSON file a person edited.
inline edge_extension parse_edge_extension(std::string_view s)
{
	if (s == "none" or s == "off")
		return edge_extension::none;
	if (s == "clamp")
		return edge_extension::clamp;
	return edge_extension::fade;
}

} // namespace wivrn::view_geometry
