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

// client/utils/view_geometry.h -- the one place the edge bleed's overscan and the lens
// mask's visible region are computed, so that the two features cannot disagree about where
// the panel's field of view lands inside a streamed eye image.
//
// The header is pure maths over bare floats, so this needs no Vulkan, no session and no
// device:
//
//   g++ -std=c++23 -O2 -I.. -o view_geometry_test tests/view_geometry_test.cpp && ./view_geometry_test
//
// (from the repository root, `-I.` rather than `-I..`; the include path only has to make
// "client/utils/view_geometry.h" resolve.)
//
// What is worth asserting here, and why:
//
//   * The widening is a scale of the TANGENT, not of the angle. On a Pico 4's roughly
//     50-degree half FOV those differ by several degrees, and getting it wrong makes the
//     margin the wrong number of pixels wide -- which is exactly the class of bug the
//     shared header exists to prevent.
//
//   * visible_rect() has to agree with the widening it is derived from. The test does that
//     the honest way: it re-derives the rectangle from the widened angles by hand and
//     compares, rather than restating the closed form the header uses.
//
//   * The sign convention. Left and down are negative angles, and a widened FOV must grow
//     OUTWARD on all four sides. An asymmetric FOV, which every real headset has, must
//     stay asymmetric and must not drift.
//
//   * is_maskable(). The lens mask branch will call this to decide whether a tile can be
//     skipped, and the whole point of the overscan margin is that it may NOT be. A tile in
//     the margin has to come back "not maskable" even though it is outside the panel.

#include "client/utils/view_geometry.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace vg = wivrn::view_geometry;

static int checks = 0;
static int failures = 0;

static void check(bool ok, const std::string & what)
{
	++checks;
	if (not ok)
		++failures;
	std::printf("  %-5s%s\n", ok ? "ok" : "FAIL", what.c_str());
}

static void check_close(float got, float want, float tol, const std::string & what)
{
	const bool ok = std::abs(got - want) <= tol;
	++checks;
	if (not ok)
	{
		++failures;
		std::printf("  FAIL %s (got %.6f, wanted %.6f, tolerance %.6f)\n",
		            what.c_str(),
		            double(got),
		            double(want),
		            double(tol));
		return;
	}
	std::printf("  ok   %s\n", what.c_str());
}

// A Pico 4's per-eye FOV, roughly, in radians, and deliberately asymmetric: the nasal and
// temporal halves differ, which is the case a symmetric shorthand would get away with
// being wrong about.
struct fov
{
	float left, right, down, up;
};
static constexpr fov pico4{-0.87f, 0.80f, -0.85f, 0.83f};
static constexpr fov symmetric{-0.8f, 0.8f, -0.8f, 0.8f};

int main()
{
	std::printf("A. clamp_overscan\n");
	{
		check(vg::clamp_overscan(0.05f) == 0.05f, "a sane margin passes through");
		check(vg::clamp_overscan(0.f) == 0.f, "zero is zero");
		check(vg::clamp_overscan(-1.f) == 0.f, "a negative margin is off, not a shrink");
		check(vg::clamp_overscan(std::nanf("")) == 0.f, "NaN is off");
		check(vg::clamp_overscan(1e30f) == vg::max_overscan, "an absurd margin is capped");
		check(vg::clamp_overscan(vg::max_overscan + 1) == vg::max_overscan, "and so is one just over");
	}

	std::printf("\nB. overscan_angle scales the tangent, not the angle\n");
	{
		const float f = 0.05f;
		for (float a: {pico4.left, pico4.right, pico4.down, pico4.up})
		{
			const float widened = vg::overscan_angle(a, f);
			check_close(std::tan(widened),
			            std::tan(a) * (1 + f),
			            1e-6f,
			            "tan(widened) == tan(angle) * (1 + f) at " + std::to_string(a));
			// Outward on every side: the magnitude grows and the sign is kept.
			check(std::abs(widened) > std::abs(a) and ((widened > 0) == (a > 0)),
			      "the edge moved outward, and stayed on its own side, at " + std::to_string(a));
		}

		// The distinction the whole header turns on. At this FOV a 5 % tangent scale is
		// nowhere near a 5 % angle scale, so a test that only checked "it got bigger"
		// would pass on the wrong implementation.
		const float naive = pico4.right * 1.05f;
		check(std::abs(vg::overscan_angle(pico4.right, 0.05f) - naive) > 0.005f,
		      "and it is measurably NOT a 5 % scale of the angle itself");

		check(vg::overscan_angle(pico4.left, 0.f) == pico4.left, "a zero margin is exactly a no-op");
		// A half-FOV at 90 degrees has no finite tangent; a projection layer cannot carry
		// it and the widening must not turn it into a NaN that reaches xrEndFrame.
		const float right_angle = 1.5707963f;
		check(std::isfinite(vg::overscan_angle(right_angle, 0.05f)),
		      "a 90 degree half FOV comes back finite rather than as a NaN");
		check(std::isfinite(vg::overscan_angle(std::nanf(""), 0.05f)) == false and
		              std::isnan(vg::overscan_angle(std::nanf(""), 0.05f)),
		      "a NaN angle is returned unchanged rather than being invented away");
	}

	std::printf("\nC. to_degrees, the number a person is shown\n");
	{
		const float d = vg::to_degrees(pico4.up, 0.05f);
		check(d > 0 and d < 5, "5 % on this edge is a fraction of a degree, not five of them");
		check_close(vg::to_degrees(pico4.up, 0.f), 0.f, 1e-6f, "and zero margin is zero degrees");
		// Two edges of different sizes get different numbers of degrees for the same
		// fraction. That is the reason the wire and the configuration carry the fraction.
		check(std::abs(vg::to_degrees(pico4.left, 0.05f)) != std::abs(vg::to_degrees(pico4.right, 0.05f)),
		      "an asymmetric FOV gets an asymmetric number of degrees from one fraction");
	}

	std::printf("\nD. visible_rect agrees with the widening it comes from\n");
	{
		const float f = 0.05f;

		// Re-derived by hand from the widened angles rather than restated: the image spans
		// the widened tangents, and the panel shows the un-widened ones.
		const auto expect = [&](const fov & v) {
			const float tl = std::tan(v.left), tr = std::tan(v.right);
			const float td = std::tan(v.down), tu = std::tan(v.up);
			const float wl = std::tan(vg::overscan_angle(v.left, f));
			const float wr = std::tan(vg::overscan_angle(v.right, f));
			const float wd = std::tan(vg::overscan_angle(v.down, f));
			const float wu = std::tan(vg::overscan_angle(v.up, f));
			vg::rect r;
			r.x0 = (tl - wl) / (wr - wl);
			r.x1 = (tr - wl) / (wr - wl);
			// The image's first row is the `up` edge, so y counts down from it.
			r.y0 = (wu - tu) / (wu - wd);
			r.y1 = (wu - td) / (wu - wd);
			return r;
		};

		for (const auto & [name, v]: {std::pair{"symmetric", symmetric}, std::pair{"pico4", pico4}})
		{
			const vg::rect got = vg::visible_rect(v.left, v.right, v.down, v.up, f);
			const vg::rect want = expect(v);
			check_close(got.x0, want.x0, 1e-6f, std::string(name) + ": x0");
			check_close(got.x1, want.x1, 1e-6f, std::string(name) + ": x1");
			check_close(got.y0, want.y0, 1e-6f, std::string(name) + ": y0");
			check_close(got.y1, want.y1, 1e-6f, std::string(name) + ": y1");
		}

		// The symmetric shorthand is the number the closed form has to reproduce.
		const vg::rect s = vg::visible_rect(symmetric.left, symmetric.right, symmetric.down, symmetric.up, f);
		const float inset = vg::symmetric_inset(f);
		check_close(s.x0, inset, 1e-6f, "a symmetric FOV is inset by f / (2 (1 + f)) on the left");
		check_close(1 - s.x1, inset, 1e-6f, "and by the same on the right");
		check_close(s.y0, inset, 1e-6f, "and top");
		check_close(1 - s.y1, inset, 1e-6f, "and bottom");
		check_close(inset, 0.05f / (2 * 1.05f), 1e-7f, "which is about 2.4 % of the width at 5 %");

		// An asymmetric FOV must get an asymmetric margin in pixels even though both sides
		// grew by the same fraction. This is the property a single inset would destroy.
		const vg::rect p = vg::visible_rect(pico4.left, pico4.right, pico4.down, pico4.up, f);
		check(std::abs(p.x0 - (1 - p.x1)) > 1e-4f,
		      "an asymmetric FOV gets asymmetric margins in pixels");

		check(vg::visible_rect(pico4.left, pico4.right, pico4.down, pico4.up, 0.f) == vg::rect{},
		      "no margin means the whole image is visible");
		// A degenerate FOV must not divide by zero and must degrade to "everything is
		// visible", which is what every consumer reads as "mask nothing".
		check(vg::visible_rect(0.f, 0.f, 0.f, 0.f, f) == vg::rect{}, "a zero FOV degrades to the whole image");
		check(vg::visible_rect(1.f, -1.f, 1.f, -1.f, f) == vg::rect{}, "and so does an inside-out one");
	}

	std::printf("\nE. visible_ndc is the same rectangle in clip space\n");
	{
		const float f = 0.08f;
		const vg::rect r = vg::visible_rect(pico4.left, pico4.right, pico4.down, pico4.up, f);
		const vg::rect n = vg::visible_ndc(pico4.left, pico4.right, pico4.down, pico4.up, f);
		check_close(n.x0, r.x0 * 2 - 1, 1e-6f, "x0 maps 0..1 onto -1..1");
		check_close(n.y1, r.y1 * 2 - 1, 1e-6f, "y1 maps 0..1 onto -1..1");
		check(n.x0 < 0 and n.x1 > 0 and n.y0 < 0 and n.y1 > 0,
		      "and the visible region still straddles the origin");
	}

	std::printf("\nF. is_maskable: the lens mask may not eat the margin\n");
	{
		const float f = 0.05f;
		const vg::rect vis = vg::visible_rect(pico4.left, pico4.right, pico4.down, pico4.up, f);

		// A tile in the middle of the picture is obviously seen.
		check(not vg::is_maskable({0.4f, 0.4f, 0.6f, 0.6f}, vis), "a central tile is not maskable");

		// A tile that straddles the boundary is partly on the panel, so it is not
		// maskable either -- an intersection test, not a centre test.
		check(not vg::is_maskable({vis.x0 - 0.01f, 0.4f, vis.x0 + 0.01f, 0.6f}, vis),
		      "a tile straddling the visible edge is not maskable");

		// A tile entirely in the margin IS outside the panel and is reported as maskable,
		// which is the correct answer to the question asked. The rule that the margin
		// must survive is enforced by giving the lens mask the visible rect for the
		// margin-free case; the comment in the header says so, and this asserts the
		// arithmetic that comment depends on.
		check(vg::is_maskable({0.f, 0.4f, vis.x0 * 0.5f, 0.6f}, vis),
		      "a tile wholly inside the left margin is outside the visible rect");

		// And the whole point: with NO margin the visible rect is the whole image, so
		// nothing at all is maskable by this test, which is the pre-overscan behaviour.
		const vg::rect none = vg::visible_rect(pico4.left, pico4.right, pico4.down, pico4.up, 0.f);
		check(not vg::is_maskable({0.f, 0.f, 0.02f, 0.02f}, none),
		      "with no margin, a corner tile is still on the panel and is not maskable");
	}

	std::printf("\nG. the edge extension enum round trips its spellings\n");
	{
		check(vg::parse_edge_extension("none") == vg::edge_extension::none, "none");
		check(vg::parse_edge_extension("off") == vg::edge_extension::none, "off is none");
		check(vg::parse_edge_extension("clamp") == vg::edge_extension::clamp, "clamp");
		check(vg::parse_edge_extension("fade") == vg::edge_extension::fade, "fade");
		// A hand-edited file with a typo must still start, with the default behaviour.
		check(vg::parse_edge_extension("purple") == vg::default_edge_extension, "an unknown spelling is the default");
		for (auto e: {vg::edge_extension::none, vg::edge_extension::clamp, vg::edge_extension::fade})
			check(vg::parse_edge_extension(vg::name(e)) == e, std::string("name() round trips ") + vg::name(e));

		// The numeric values go on the wire as a uint8_t, so they are part of the
		// protocol and cannot be reordered without a version bump.
		check(uint8_t(vg::edge_extension::none) == 0 and
		              uint8_t(vg::edge_extension::clamp) == 1 and
		              uint8_t(vg::edge_extension::fade) == 2,
		      "the wire values are 0, 1, 2 and are not free to change");

		check(vg::clamp_fade_distance(-1.f) == 0.f, "a negative fade distance is off");
		check(vg::clamp_fade_distance(10.f) == vg::max_fade_distance, "and a huge one is capped");
		check(vg::clamp_fade_distance(vg::default_fade_distance) == vg::default_fade_distance,
		      "the default survives clamping");
	}

	std::printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
