// The lens mask geometry: which 64x64 tiles of an encoded eye image the optics can never show.
//
// client/utils/view_geometry.h is header-only and links nothing, which is the point of it: the
// same function decides what the compositor paints flat, what the NX Warp encoder tells the codec
// to skip, and what the edge-bleed branch must NOT let the mask eat. So it is tested on its own,
// with no Vulkan, no OpenXR and no session.
//
// Build and run:
//   g++ -std=c++23 -O2 -I. -o lens_mask_test tests/lens_mask_test.cpp && ./lens_mask_test
//
// Beyond the assertions it PRINTS a table, because the number that matters operationally --
// how many of a Pico 4's 289 tiles per eye are masked -- depends on the foveation curve in
// force, and a table is the honest way to say that.

#include "client/utils/view_geometry.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <numbers>
#include <vector>

using namespace wivrn::view_geometry;

static fov_angles symmetric(double total_degrees)
{
	const float h = float(total_degrees * 0.5 * std::numbers::pi / 180.0);
	return {-h, h, h, -h};
}

// --- a model of the compositor's NEUTRAL foveation curve --------------------------------
//
// server/compositor/foveation.cpp builds its runs from a single-tan curve at strength 0:
//
//     defoveate(x) = lambda/a * tan(a*x + b) + c,   a, b solved so defoveate(+-1) = +-1
//
// with lambda = foveated_dim / source_dim and c the fovea centre in source coordinates. This
// reproduces it at c = 0 (a centred gaze), which is the only case a test with no eye tracker
// can speak for. It is a MODEL, not the compositor's own code: it exists so the table below
// can quote a realistic run distribution rather than only the identity one, and nothing in the
// shipped path calls it.
static void solve_neutral(double lambda, double & a_out, double & b_out)
{
	auto eq = [lambda](double a) { return 2 * std::atan(a / lambda) - 2 * a; };
	double lo = 1e-9, hi = 1;
	while (eq(hi) > 0)
		hi *= 2;
	for (int i = 0; i < 200; ++i)
	{
		const double m = 0.5 * (lo + hi);
		(eq(m) > 0 ? lo : hi) = m;
	}
	a_out = 0.5 * (lo + hi);
	b_out = 0;
}

static std::vector<uint16_t> neutral_runs(uint32_t foveated_dim, uint32_t source_dim)
{
	if (foveated_dim >= source_dim)
		return {uint16_t(source_dim)};

	const double lambda = double(foveated_dim) / source_dim;
	double a = 0, b = 0;
	solve_neutral(lambda, a, b);

	std::vector<uint16_t> left, right;
	uint32_t last = 0;
	for (uint32_t i = 1; i < foveated_dim; ++i)
	{
		const double u = (i * 2.0) / foveated_dim - 1;
		const double f = lambda / a * std::tan(a * u + b);
		uint32_t n = uint32_t(std::clamp((f * 0.5 + 0.5) * source_dim + 0.5, 0.0, double(source_dim)));
		if (n <= last)
			n = last + 1;
		const size_t count = n - last;
		auto & vec = u < 0 ? left : right;
		if (count > vec.size())
			vec.resize(count);
		vec[count - 1]++;
		last = n;
	}
	size_t count = source_dim > last ? source_dim - last : 1;
	if (count > right.size())
		right.resize(count);
	right[count - 1]++;

	const size_t n = std::max(left.size(), right.size());
	std::vector<uint16_t> out(n - left.size(), 0);
	out.insert(out.end(), left.rbegin(), left.rend());
	if (not right.empty())
		out.back() = uint16_t(out.back() + right.front());
	if (right.size() > 1)
		out.insert(out.end(), right.begin() + 1, right.end());
	out.resize(n * 2 - 1);
	return out;
}

// --dump <fov-degrees-h> <fov-degrees-v> <width> <height> <source-w> <source-h> [margin]
//
// Print the mask as a grid of 0/1 rows, plus a header line of numbers, for a renderer to
// draw over a real frame. It is the same lens_tile_mask() the server calls, so the picture
// in docs/assets is the mask that shipped and not a drawing of it.
static int dump(int argc, char ** argv)
{
	auto arg = [&](int i, double d) { return i < argc ? std::atof(argv[i]) : d; };
	const double fh = arg(2, 100), fv = arg(3, 100);
	const uint32_t w = uint32_t(arg(4, 1088)), h = uint32_t(arg(5, 1088));
	const uint32_t sw = uint32_t(arg(6, w)), sh = uint32_t(arg(7, h));
	const uint32_t margin = uint32_t(arg(8, 1));

	const float hx = float(fh * 0.5 * std::numbers::pi / 180.0);
	const float hy = float(fv * 0.5 * std::numbers::pi / 180.0);
	const fov_angles fov{-hx, hx, hy, -hy};
	const auto rx = neutral_runs(w, sw), ry = neutral_runs(h, sh);
	const auto m = lens_tile_mask(fov, rx, ry, w, h, {.margin_tiles = margin});
	const auto m0 = lens_tile_mask(fov, rx, ry, w, h, {.margin_tiles = 0});
	const auto e = visible_region(fov);
	const auto r = fov_bounds(fov);

	// cols rows masked total  rx ry  rect_x0 rect_x1 rect_y0 rect_y1
	std::printf("%u %u %u %u %.6f %.6f %.6f %.6f %.6f %.6f\n", m.cols, m.rows, m.masked,
	            m.total(), e.rx, e.ry, r.x0, r.x1, r.y0, r.y1);
	for (uint32_t y = 0; y < m.rows; ++y)
	{
		for (uint32_t x = 0; x < m.cols; ++x)
			// 2 = masked, 1 = outside the region but kept by the margin ring, 0 = kept
			std::putchar(m.at(x, y) ? '2' : (m0.at(x, y) ? '1' : '0'));
		std::putchar('\n');
	}
	return 0;
}

int main(int argc, char ** argv)
{
	if (argc > 1 and std::string(argv[1]) == "--dump")
		return dump(argc, argv);

	// --- 1. the synthetic case the task names -------------------------------------------
	//
	// A symmetric 100 degree FOV, no foveation, 1088x1088 -- a 17x17 grid. The four corners
	// must be masked and the centre must not, and that must survive the default one-tile
	// margin ring.
	{
		const auto m = lens_tile_mask(symmetric(100), {}, {}, 1088, 1088);
		assert(m.cols == 17 and m.rows == 17);
		assert(m.at(0, 0) and m.at(16, 0) and m.at(0, 16) and m.at(16, 16));
		assert(not m.at(8, 8));
		// The centre row and column are the widest part of the ellipse: nothing on them
		// may be masked, at any margin.
		for (uint32_t i = 0; i < 17; ++i)
		{
			assert(not m.at(i, 8));
			assert(not m.at(8, i));
		}
		std::printf("100 deg symmetric, no foveation, 1088x1088: %u of %u masked\n",
		            m.masked, m.total());
		assert(m.masked > 0);
	}

	// --- 2. the margin is a margin ------------------------------------------------------
	//
	// A larger ring masks strictly less, and margin 0 masks strictly more than the default.
	{
		const auto m0 = lens_tile_mask(symmetric(100), {}, {}, 1088, 1088, {.margin_tiles = 0});
		const auto m1 = lens_tile_mask(symmetric(100), {}, {}, 1088, 1088, {.margin_tiles = 1});
		const auto m2 = lens_tile_mask(symmetric(100), {}, {}, 1088, 1088, {.margin_tiles = 2});
		assert(m0.masked > m1.masked);
		assert(m1.masked > m2.masked);
		// Every masked tile at a wider margin is masked at a narrower one: the margin
		// only ever removes tiles from the set.
		for (size_t i = 0; i < m1.tiles.size(); ++i)
		{
			assert(not m2.tiles[i] or m1.tiles[i]);
			assert(not m1.tiles[i] or m0.tiles[i]);
		}
		std::printf("margin 0/1/2 tiles: %u / %u / %u masked of %u\n",
		            m0.masked, m1.masked, m2.masked, m1.total());
	}

	// --- 3. edge bleed and the lens mask, on one model -----------------------------------
	//
	// The server widens the FOV it hands the application by `edge_bleed.overscan`, and
	// everything downstream -- the compositor, the encoder, view_info -- sees the WIDENED
	// one. The mask must not eat the ring that widening paid for.
	{
		const fov_angles display = symmetric(100);
		const float f = default_overscan; // 0.05, edge bleed's own setting

		// (a) the two spellings of the same widening are the same widening. This is what
		// makes it ONE model rather than two that happen to agree today: the ellipse's
		// growth factor and overscan_angle()'s tangent scaling are the same number.
		const auto grown = visible_region(display, f);
		const auto widened = visible_region(widen(display, f), 0.f);
		assert(std::abs(grown.rx - widened.rx) < 1e-6 * grown.rx);
		assert(std::abs(grown.ry - widened.ry) < 1e-6 * grown.ry);
		assert(std::abs(grown.rx - visible_region(display, 0.f).rx * (1.0 + f)) < 1e-9);

		// (b) THE MASK IS INVARIANT UNDER OVERSCAN. The encoded size does not change and
		// the rectangle and the ellipse both scale by (1 + f), so every tile lands in the
		// same place relative to the region. Turning edge bleed on cannot mask one extra
		// tile of the ring it just bought, which is the guarantee is_maskable()'s comment
		// asks for -- met by construction here rather than by a veto.
		const auto off = lens_tile_mask(display, {}, {}, 1088, 1088);
		const auto on = lens_tile_mask(widen(display, f), {}, {}, 1088, 1088);
		assert(on.masked == off.masked);
		assert(on.tiles == off.tiles);

		// and the same said the other way round, for a caller holding the display FOV:
		const auto with_margin = lens_tile_mask(display, {}, {}, 1088, 1088,
		                                        {.overscan_margin = f});
		assert(with_margin.masked < off.masked); // a smaller mask, never a larger one
		for (size_t i = 0; i < with_margin.tiles.size(); ++i)
			assert(not with_margin.tiles[i] or off.tiles[i]);

		// (c) NO TILE THE OVERSCAN RING CAN PUT ON THE PANEL IS MASKED. The ring is
		// everything outside visible_rect(); the part of it a late reprojection can reach
		// is the part inside the lens region grown by f. Every masked tile is outside
		// that, so no masked tile is reachable.
		const rect vis = visible_rect(display.left, display.right, display.down, display.up, f);
		const fov_angles wide = widen(display, f);
		const auto e = visible_region(wide, 0.f);
		const auto r = fov_bounds(wide);
		uint32_t ring_masked = 0, panel_masked = 0;
		for (uint32_t ty = 0; ty < on.rows; ++ty)
		{
			for (uint32_t tx = 0; tx < on.cols; ++tx)
			{
				const double x0 = double(tx * 64) / 1088, x1 = std::min(1.0, double((tx + 1) * 64) / 1088);
				const double y0 = double(ty * 64) / 1088, y1 = std::min(1.0, double((ty + 1) * 64) / 1088);
				const tan_rect t{r.x0 + x0 * (r.x1 - r.x0), r.x0 + x1 * (r.x1 - r.x0),
				                 r.y0 + y0 * (r.y1 - r.y0), r.y0 + y1 * (r.y1 - r.y0)};
				const rect nr{float(x0), float(y0), float(x1), float(y1)};
				if (not on.at(tx, ty))
					continue;
				// masked => outside the reachable region, whatever else is true
				assert(not region_covers(e, t));
				if (is_maskable(nr, vis))
					++ring_masked;
				else
					++panel_masked;
			}
		}
		// Every masked tile that is NOT in the ring overlaps the panel's bounding box:
		// those are the corners of the rectangle that the round lens does not show, and
		// they are exactly the extra thing the ellipse knows over is_maskable(). They are
		// masked at overscan 0 too -- (b) -- so edge bleed did not create them.
		assert(panel_masked > 0);
		std::printf("overscan %.2f: %u masked (%u in the ring, %u in the panel rect), "
		            "identical to overscan 0\n",
		            double(f), on.masked, ring_masked, panel_masked);
	}

	// --- 4. the region test is not a corner test ----------------------------------------
	//
	// A rectangle straddling the region has all four corners outside it and must still be
	// reported as covered. Getting this wrong masks a tile the eye can see.
	{
		const ellipse e{0, 0, 1, 1};
		assert(region_covers(e, tan_rect{-2, 2, -0.1, 0.1}));
		assert(region_covers(e, tan_rect{-0.1, 0.1, -2, 2}));
		assert(not region_covers(e, tan_rect{0.8, 2.0, 0.8, 2.0}));
		assert(region_covers(e, tan_rect{-0.5, 0.5, -0.5, 0.5}));
	}

	// --- 5. the foveation remap -----------------------------------------------------------
	{
		// The identity: no runs at all, and a single run of 1:1 pixels, agree.
		const auto a = foveation_bounds({}, 8);
		std::vector<uint16_t> one{8};
		const auto b = foveation_bounds(one, 8);
		assert(a == b);
		assert(a.back() == 8);

		// 1,4,5,3,1 is the example in wivrn_packets.h: the first destination pixel takes
		// 3 source pixels, the next 4 take 2, then 5 take 1, then 3 take 2, then 1 takes 3.
		std::vector<uint16_t> runs{1, 4, 5, 3, 1};
		const auto c = foveation_bounds(runs, 14);
		assert(c[1] - c[0] == 3);
		assert(c[2] - c[1] == 2);
		assert(c[6] - c[5] == 1);
		assert(c[14] - c[13] == 3);
		assert(c[14] == 3 + 4 * 2 + 5 + 3 * 2 + 3);
	}

	// --- 6. foveation shrinks the mask, and never grows it --------------------------------
	//
	// A foveated corner tile covers MORE source area than an unfoveated one, so it reaches
	// the visible region sooner. Fewer tiles are masked, and never a tile the identity
	// mapping did not already mask.
	{
		const auto id = lens_tile_mask(symmetric(100), {}, {}, 1088, 1088);
		const auto rx = neutral_runs(1088, 2176);
		const auto fv = lens_tile_mask(symmetric(100), rx, rx, 1088, 1088);
		assert(fv.masked <= id.masked);
		for (size_t i = 0; i < fv.tiles.size(); ++i)
			assert(not fv.tiles[i] or id.tiles[i]);
	}

	// --- 7. degenerate inputs mask nothing rather than everything --------------------------
	{
		assert(lens_tile_mask({0, 0, 0, 0}, {}, {}, 1088, 1088).masked == 0);
		assert(lens_tile_mask(symmetric(100), {}, {}, 0, 0).total() == 0);
	}

	// --- the table -------------------------------------------------------------------------
	//
	// 1088x1088 per eye is the Pico 4's NX Warp geometry (17x17 = 289 tiles per eye, 578 for
	// the pair). The FOV column is the runtime's per-eye view FOV; the server logs the real
	// one once per session ("nxwarp: lens mask ..."), because a headset's view configuration
	// is the only authority on it and it is not in any capture in this tree.
	// A SYMMETRIC FOV gives the same answer at every angle, and that is not a bug: the
	// ellipse is inscribed in the FOV rectangle, so widening the FOV scales the rectangle and
	// the ellipse together and moves no tile across the boundary. What the FOV actually
	// decides is the rectangle's ASPECT and its ASYMMETRY -- which is why the table varies the
	// aspect too, and why the foveation column is the one that moves the number.
	std::printf("\n1088x1088 per eye, 17x17 = 289 tiles\n");
	std::printf("%-16s %-22s %10s %10s\n", "FOV", "foveation", "margin 0", "margin 1");
	struct
	{
		const char * name;
		fov_angles fov;
	} fovs[] = {
	        {"100 sym", symmetric(100)},
	        {"105 sym", symmetric(105)},
	        {"105x98", fov_angles{-0.916f, 0.916f, 0.855f, -0.855f}},
	        {"asym 105", fov_angles{-1.000f, 0.833f, 0.916f, -0.916f}},
	};
	for (const auto & f: fovs)
	{
		for (uint32_t src: {1088u, 1300u, 1554u, 2176u})
		{
			const auto runs = neutral_runs(1088, src);
			const auto m0 = lens_tile_mask(f.fov, runs, runs, 1088, 1088, {.margin_tiles = 0});
			const auto m1 = lens_tile_mask(f.fov, runs, runs, 1088, 1088, {.margin_tiles = 1});
			char what[64];
			if (src == 1088)
				std::snprintf(what, sizeof what, "none (1:1)");
			else
				std::snprintf(what, sizeof what, "neutral, source %u", src);
			std::printf("%-16s %-22s %10u %10u\n", f.name, what, m0.masked, m1.masked);
		}
	}

	std::printf("\nlens_mask_test: OK\n");
	return 0;
}
