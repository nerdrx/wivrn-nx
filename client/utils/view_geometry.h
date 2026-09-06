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
//     undo the feature it was added for. `visible_region()` below is therefore GROWN by the
//     same margin, and the consequence is that the mask does not change at all when
//     overscan is turned on; `is_maskable()` remains the rectangular question the
//     reprojection pass asks, and is a different one -- see "one tangent-space model" below
//     for why the two are not the same test and must not be ANDed.
//
// Header only, no Vulkan, no Qt, no OpenXR: the server (server/driver/wivrn_hmd.cpp, the
// compositor's foveation pass, the NX Warp encoder) and the client
// (client/scenes/stream_defoveator.cpp) all include it, and tests/view_geometry_test.cpp,
// tests/edge_bleed_test.cpp and tests/lens_mask_test.cpp check it. Angles are the signed
// half angles both XrFovf and xrt_fov use -- left and down negative, right and up positive
// -- passed as bare floats so neither type has to be visible here.
//
// ONE TANGENT-SPACE MODEL, TWO SHAPES OVER IT.
//
// Everything below works on tan(angle) and not on the angle, because that is the space the
// rendered image is linear in: a projection layer maps its pixel grid affinely onto
// tan(angle), so a fraction of the plane is a fraction of the pixels whatever the FOV is.
// The two shapes are
//
//   * the RECTANGLE, which is the picture: `visible_rect()` says where the display FOV
//     lands inside an overscanned image, and `is_maskable()` asks whether a region is
//     disjoint from it. This is what the reprojection pass needs, and it is the panel's
//     BOUNDING BOX;
//   * the ELLIPSE, which is the optics: `visible_region()` is what a lens can actually
//     show, because the barrel is round and the corners of the rendered rectangle are
//     never presented to an eye. This is what the lens mask needs.
//
// The ellipse is strictly inside the rectangle, so the lens mask masks MORE than the
// rectangular test alone would: the four corners of the panel's own bounding box are inside
// `visible_rect()` and outside the lens. That is not a disagreement between the two, it is
// the extra thing the round shape knows, and the two are NOT ANDed -- requiring
// `is_maskable()` as well would mask nothing at all whenever overscan is off, because with
// no margin the panel rectangle is the whole image.
//
// What holds instead, and what tests/lens_mask_test.cpp asserts, is stronger than an
// intersection would be: THE LENS MASK IS EXACTLY INVARIANT UNDER OVERSCAN. Widening the
// FOV scales the tangent rectangle and the protected ellipse by the same (1 + f), and the
// encoded size does not change, so every tile lands in the same place relative to the
// region and the mask is identical tile for tile at overscan 0 and at 0.05. Turning edge
// bleed on therefore cannot mask one extra pixel of the ring it just paid for, which is the
// guarantee `is_maskable()`'s comment is asking for, met by construction rather than by a
// veto.
//
// The overscan margin is the single number both halves grow by. `overscan_angle()` widens
// an edge by scaling its tangent, and `visible_region()` grows its radii by the same
// factor, so
//
//     visible_region(display_fov, f) == visible_region(widen(display_fov, f), 0)
//
// exactly. tests/lens_mask_test.cpp asserts that identity rather than leaving it as a
// claim, and it is what lets the NX Warp encoder -- which receives the ALREADY WIDENED FOV
// in view_info, because the overscan is applied at wivrn_hmd::get_view_poses() long before
// anything is encoded -- pass a margin of 0 and still get the overscan-protected region.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

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

// --- the lens: what the optics can show -------------------------------------------------
//
// `visible_rect()` above is the panel's bounding box. A lens shows a ROUND region inside
// it, and the corners of the rendered rectangle fall outside the barrel and never reach an
// eye. That is the difference the lens mask lives on, and it is why this is an ellipse and
// not a fifth rectangle.

// Per-eye field of view, in radians, in the same sign convention as everything above.
// Deliberately not XrFovf or xrt_fov: this header is included by translation units that
// have neither.
struct fov_angles
{
	float left = 0;
	float right = 0;
	float up = 0;
	float down = 0;
};

// An axis-aligned rectangle in TANGENT space. Distinct from `rect`, which is normalised
// image coordinates: the two are related by an affine map and confusing them is the bug
// this type exists to prevent.
struct tan_rect
{
	double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
};

// An axis-aligned ellipse in tangent space.
struct ellipse
{
	double cx = 0, cy = 0, rx = 0, ry = 0;
};

// The tangent-space bounding box of a field of view.
inline tan_rect fov_bounds(const fov_angles & fov)
{
	return {
	        std::tan(double(fov.left)),
	        std::tan(double(fov.right)),
	        std::tan(double(fov.down)),
	        std::tan(double(fov.up)),
	};
}

// A whole FOV widened by `fraction`, edge by edge, through overscan_angle(). This is the
// server's own widening (wivrn_hmd::get_view_poses) expressed on the struct, so that the
// identity in the header comment can be written down and tested rather than asserted.
inline fov_angles widen(const fov_angles & fov, float fraction)
{
	return {
	        overscan_angle(fov.left, fraction),
	        overscan_angle(fov.right, fraction),
	        overscan_angle(fov.up, fraction),
	        overscan_angle(fov.down, fraction),
	};
}

// The region of a rendered eye image the optics can present, grown by `overscan_margin`.
//
// Centred on the lens axis -- tangent-space origin, which is where the view pose points --
// with a half-extent on each axis taken from the LARGER of that axis's two half-angles.
//
// TAKING THE MAX RATHER THAN THE HALF-EXTENT IS THE CONSERVATIVE CHOICE, and it is where an
// asymmetric FOV earns its keep. A headset's FOV is asymmetric because the panel is offset
// against the lens; the round region is still centred on the axis. Sizing the ellipse from
// the larger half on each axis means it reaches at least as far as the picture does on the
// wide side and covers the narrow side entirely, so only the far corners of the wide side
// can be masked -- which is where a lens genuinely runs out. Inscribing an ellipse in the
// rectangle instead would recentre the region on the picture rather than on the lens, and
// would then give an answer that does not depend on the FOV at all, which is how you can
// tell it is the wrong model.
//
// It errs toward showing too much: a real lens shows less than this near the ellipse's own
// edge. `margin_tiles` below is the second, cruder guard on top of that.
//
// `overscan_margin` is the SAME fraction `default_overscan` names and `overscan_angle()`
// applies, sanitised the same way. Pass it when you hold the DISPLAY fov; pass 0 when you
// hold a FOV that has already been widened, which is what arrives in view_info.
inline ellipse visible_region(const fov_angles & fov, float overscan_margin = 0.f)
{
	const tan_rect r = fov_bounds(fov);
	const double grow = 1.0 + double(clamp_overscan(overscan_margin));
	return {
	        0.0,
	        0.0,
	        std::max(std::abs(r.x0), std::abs(r.x1)) * grow,
	        std::max(std::abs(r.y0), std::abs(r.y1)) * grow,
	};
}

// Does any part of `r` fall inside `e`?
//
// Exact, not a corner test: scaling by (rx, ry) takes the ellipse to the unit circle and
// leaves the axis-aligned rectangle axis-aligned, so the question becomes "is the closest
// point of a rectangle to the origin within 1", which is a clamp and a dot product. A
// four-corner test would answer NO for a rectangle straddling the region -- a tile the eye
// can see -- which is the one wrong answer this must never give.
inline bool region_covers(const ellipse & e, const tan_rect & r)
{
	if (not(e.rx > 0) or not(e.ry > 0))
		return true; // a degenerate region masks nothing

	const double nx0 = (std::min(r.x0, r.x1) - e.cx) / e.rx;
	const double nx1 = (std::max(r.x0, r.x1) - e.cx) / e.rx;
	const double ny0 = (std::min(r.y0, r.y1) - e.cy) / e.ry;
	const double ny1 = (std::max(r.y0, r.y1) - e.cy) / e.ry;

	const double qx = std::clamp(0.0, nx0, nx1);
	const double qy = std::clamp(0.0, ny0, ny1);
	return qx * qx + qy * qy <= 1.0;
}

// --- the foveation remap ---------------------------------------------------------------
//
// to_headset::foveation_parameter is a run-length list over the DESTINATION (encoded) axis:
// entry i covers `runs[i]` destination pixels, each of which consumes `|n_ratio - i| + 1`
// source pixels, where n_ratio = (runs.size() - 1) / 2. That is exactly how the
// compositor's fill_ubo walks it and how the headset's defoveator undoes it, so it is
// repeated here rather than reinterpreted.
//
// Returns dim + 1 boundaries: bounds[i] is the first source pixel of destination pixel i,
// and bounds[dim] is the source size. An empty or short run list is the identity, and a run
// list that runs out before `dim` (which is what the compositor writes when the encode is
// at least as large as the source) clamps, the same way fill_ubo pads its tail.
inline std::vector<uint32_t> foveation_bounds(std::span<const uint16_t> runs, uint32_t dim)
{
	std::vector<uint32_t> bounds;
	bounds.reserve(size_t(dim) + 1);
	bounds.push_back(0);

	if (not runs.empty())
	{
		const int n_ratio = (int(runs.size()) - 1) / 2;
		uint32_t src = 0;
		for (size_t i = 0; i < runs.size() and bounds.size() <= dim; ++i)
		{
			const uint32_t n_source = uint32_t(std::abs(n_ratio - int(i)) + 1);
			for (uint16_t j = 0; j < runs[i] and bounds.size() <= dim; ++j)
			{
				src += n_source;
				bounds.push_back(src);
			}
		}
	}

	// Identity tail: either there were no runs at all, or they described fewer
	// destination pixels than the image has.
	while (bounds.size() <= dim)
		bounds.push_back(bounds.back() + 1);

	return bounds;
}

// --- the tile mask -----------------------------------------------------------------------

struct tile_mask_options
{
	// The codec's tile side, in encoded pixels. nxvc is 64x64 and so is the foveation
	// pass's own alignment (server/encoder/stream_scale.h).
	uint32_t tile = 64;

	// How many tiles of slack to leave around the visible region, in tiles. A tile is
	// masked only when it AND every tile within this Chebyshev distance of it are outside
	// the region, so at the default 1 the whole ring of tiles touching the boundary is
	// left coded. It is the guard against everything this geometry does not model: lens
	// tolerances, an eye that is not on the optical axis, the runtime handing out a FOV
	// slightly larger than the optics, and the resampling the defoveator does at a tile
	// edge.
	uint32_t margin_tiles = 1;

	// The overscan the FOV has NOT already been widened by. 0 -- the default -- is right
	// for a caller holding a FOV that arrived through view_info, because the server widens
	// at wivrn_hmd::get_view_poses() and everything downstream sees the widened one. A
	// caller holding the DISPLAY fov passes the configured `edge_bleed.overscan` here.
	float overscan_margin = 0.f;

};

struct tile_mask
{
	uint32_t cols = 0;
	uint32_t rows = 0;
	// 1 = the whole tile is outside the visible region, with the margin applied.
	std::vector<uint8_t> tiles;
	uint32_t masked = 0;

	uint32_t total() const
	{
		return cols * rows;
	}

	bool at(uint32_t x, uint32_t y) const
	{
		return x < cols and y < rows and tiles[size_t(y) * cols + x];
	}

	bool empty() const
	{
		return masked == 0;
	}
};

// The tiles of ONE EYE's encoded image that the lens can never show.
//
// `width`/`height` are the encoded per-eye size in pixels; `fov_x`/`fov_y` are that eye's
// foveation runs as they go on the wire. A caller with no foveation passes empty spans.
inline tile_mask lens_tile_mask(const fov_angles & fov,
                                std::span<const uint16_t> fov_x,
                                std::span<const uint16_t> fov_y,
                                uint32_t width,
                                uint32_t height,
                                const tile_mask_options & opt = {})
{
	tile_mask out;
	const uint32_t tile = opt.tile ? opt.tile : 64;
	if (width == 0 or height == 0)
		return out;

	out.cols = (width + tile - 1) / tile;
	out.rows = (height + tile - 1) / tile;
	out.tiles.assign(size_t(out.cols) * out.rows, 0);

	const auto bx = foveation_bounds(fov_x, width);
	const auto by = foveation_bounds(fov_y, height);
	const double src_w = double(bx[width]);
	const double src_h = double(by[height]);
	if (not(src_w > 0) or not(src_h > 0))
		return out;

	const tan_rect r = fov_bounds(fov);
	const ellipse e = visible_region(fov, opt.overscan_margin);

	// The raw test, before the margin ring: is this tile's source footprint entirely
	// outside the visible region?
	std::vector<uint8_t> outside(out.tiles.size(), 0);
	for (uint32_t ty = 0; ty < out.rows; ++ty)
	{
		const uint32_t y0 = ty * tile;
		const uint32_t y1 = std::min(y0 + tile, height);
		for (uint32_t tx = 0; tx < out.cols; ++tx)
		{
			const uint32_t x0 = tx * tile;
			const uint32_t x1 = std::min(x0 + tile, width);

			// Destination pixels -> source pixels -> normalised -> tangent.
			const double u0 = bx[x0] / src_w, u1 = bx[x1] / src_w;
			const double v0 = by[y0] / src_h, v1 = by[y1] / src_h;
			const tan_rect t{
			        r.x0 + u0 * (r.x1 - r.x0),
			        r.x0 + u1 * (r.x1 - r.x0),
			        r.y0 + v0 * (r.y1 - r.y0),
			        r.y0 + v1 * (r.y1 - r.y0),
			};
			outside[size_t(ty) * out.cols + tx] = region_covers(e, t) ? 0 : 1;
		}
	}

	// Erode by the margin. A tile survives only if every tile within `margin_tiles` of it
	// is also outside; a neighbour off the grid is outside by definition, so the image's
	// own edge does not un-mask the corners.
	const int m = int(opt.margin_tiles);
	for (uint32_t ty = 0; ty < out.rows; ++ty)
	{
		for (uint32_t tx = 0; tx < out.cols; ++tx)
		{
			bool all = true;
			for (int dy = -m; dy <= m and all; ++dy)
			{
				for (int dx = -m; dx <= m and all; ++dx)
				{
					const int nx = int(tx) + dx, ny = int(ty) + dy;
					if (nx < 0 or ny < 0 or nx >= int(out.cols) or ny >= int(out.rows))
						continue;
					all = outside[size_t(ny) * out.cols + nx];
				}
			}
			if (all)
			{
				out.tiles[size_t(ty) * out.cols + tx] = 1;
				++out.masked;
			}
		}
	}

	return out;
}

} // namespace wivrn::view_geometry
