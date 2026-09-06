// The reprojection pass's vertex grid, and the one property that matters about skipping
// cells: it must never change the picture except by leaving out cells it was told to.
//
// The reference below is the ORIGINAL emission loop from stream_defoveator.cpp, copied
// verbatim rather than paraphrased. If the refactor ever drifts from it, the first test
// fails -- which is the point, because that drift would be a silent change to every frame
// the headset shows.

#include "scenes/stream_grid.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

using namespace wivrn::stream_grid;

namespace
{
int failures = 0;
void check(bool ok, const char * what)
{
	std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (not ok)
		++failures;
}

// The loop exactly as stream_defoveator.cpp had it before the extraction.
std::vector<vertex> reference(const std::vector<uint16_t> & px,
                              const std::vector<uint16_t> & py,
                              int out_w,
                              int out_h)
{
	std::vector<vertex> v;
	const int n_ratio_x = (int(px.size()) - 1) / 2;
	const int n_ratio_y = (int(py.size()) - 1) / 2;
	const float sx = 2.f / float(out_w), sy = 2.f / float(out_h);
	uint32_t in_y = 0;
	float out_y = -0.5f * float(out_h);
	for (size_t iy = 0; iy < py.size(); ++iy)
	{
		const int n_out_y = int(py[iy]);
		const int ratio_y = std::abs(n_ratio_y - int(iy)) + 1;
		uint32_t in_x = 0;
		float out_x = -0.5f * float(out_w);
		for (size_t ix = 0; ix < px.size(); ++ix)
		{
			const int n_out_x = int(px[ix]);
			const int ratio_x = std::abs(n_ratio_x - int(ix)) + 1;
			v.push_back({out_x * sx, out_y * sy, in_x, in_y});
			v.push_back({out_x * sx, (out_y + float(n_out_y * ratio_y)) * sy, in_x, in_y + uint32_t(n_out_y)});
			in_x += uint32_t(n_out_x);
			out_x += float(n_out_x * ratio_x);
		}
		v.push_back({out_x * sx, out_y * sy, in_x, in_y});
		in_y += uint32_t(n_out_y);
		out_y += float(n_out_y * ratio_y);
		v.push_back({out_x * sx, out_y * sy, in_x, in_y});
		v.push_back({out_x * sx, out_y * sy, in_x, in_y});
	}
	return v;
}

bool same(const vertex & a, const vertex & b)
{
	return a.px == b.px and a.py == b.py and a.u == b.u and a.v == b.v;
}
} // namespace

// --dump <fov-h-deg> <fov-v-deg> <cells-x> <cells-y> <overscan>
//
// The cell mask as text, for tools/render_display_grid.py to draw. It calls the same
// lens_cell_mask() the display pass calls, so the picture in docs/assets is the mask that
// ships rather than a Python drawing of one.
static int dump(int argc, char ** argv)
{
	auto arg = [&](int i, double d) { return i < argc ? std::atof(argv[i]) : d; };
	const double fh = arg(2, 100), fv = arg(3, 100);
	const int nx = int(arg(4, 15)), ny = int(arg(5, 15));
	const double overscan = arg(6, 0.0);

	// An odd run count, uniform: the shape of a neutral foveation.
	const int cx = nx % 2 ? nx : nx + 1, cy = ny % 2 ? ny : ny + 1;
	std::vector<uint16_t> px(size_t(cx), uint16_t(1)), py(size_t(cy), uint16_t(1));

	const float hx = float(fh * 0.5 * M_PI / 180.0);
	const float hy = float(fv * 0.5 * M_PI / 180.0);
	const wivrn::view_geometry::fov_angles fov{-hx, hx, -hy, hy};

	const auto m = lens_cell_mask(fov, px, py, overscan);
	const auto m0 = lens_cell_mask(fov, px, py, 0.0);
	const auto e = wivrn::view_geometry::visible_region(fov, overscan);
	const auto r = wivrn::view_geometry::fov_bounds(fov);

	std::printf("%u %u %u %u %.6f %.6f %.6f %.6f %.6f %.6f\n", m.cols, m.rows, m.masked,
	            m.cols * m.rows, e.rx, e.ry, r.x0, r.x1, r.y0, r.y1);
	for (uint32_t y = 0; y < m.rows; ++y)
	{
		for (uint32_t x = 0; x < m.cols; ++x)
			// 2 = skipped, 1 = would be skipped without the overscan ring, 0 = drawn
			std::putchar(m.at(x, y) ? '2' : (m0.at(x, y) ? '1' : '0'));
		std::putchar('\n');
	}
	return 0;
}

int main(int argc, char ** argv)
{
	if (argc > 1 and std::string_view(argv[1]) == "--dump")
		return dump(argc, argv);

	// A realistic asymmetric foveation: 7 runs each way, as the server sends.
	const std::vector<uint16_t> px{64, 96, 128, 256, 128, 96, 64};
	const std::vector<uint16_t> py{64, 96, 128, 256, 128, 96, 64};
	const int w = 1088, h = 1088;

	std::printf("no mask reproduces the original strip\n");
	{
		const auto ref = reference(px, py, w, h);
		std::vector<vertex> got(max_vertices(px.size(), py.size()));
		const size_t n = emit(px, py, w, h, {}, got.data(), got.size());
		check(n == ref.size(), "same vertex count");
		bool identical = n == ref.size();
		for (size_t i = 0; identical and i < n; ++i)
			identical = same(got[i], ref[i]);
		check(identical, "every vertex identical to the original loop");
	}

	std::printf("a mask that skips nothing is also the original strip\n");
	{
		cell_mask m{.cols = uint32_t(px.size()), .rows = uint32_t(py.size())};
		m.skip.assign(px.size() * py.size(), 0);
		const auto ref = reference(px, py, w, h);
		std::vector<vertex> got(max_vertices(px.size(), py.size()));
		const size_t n = emit(px, py, w, h, m, got.data(), got.size());
		bool identical = n == ref.size();
		for (size_t i = 0; identical and i < n; ++i)
			identical = same(got[i], ref[i]);
		check(identical, "all-zero mask changes nothing");
	}

	std::printf("a mask of the wrong shape is ignored, not misapplied\n");
	{
		cell_mask m{.cols = 3, .rows = 3};
		m.skip.assign(9, 1); // would skip everything if it were believed
		const auto ref = reference(px, py, w, h);
		std::vector<vertex> got(max_vertices(px.size(), py.size()));
		const size_t n = emit(px, py, w, h, m, got.data(), got.size());
		check(n == ref.size(), "a stale mask draws the whole grid");
	}

	std::printf("skipping the corners keeps every unmasked cell\n");
	{
		const uint32_t cols = px.size(), rows = py.size();
		cell_mask m{.cols = cols, .rows = rows};
		m.skip.assign(size_t(cols) * rows, 0);
		// The four corner cells only -- the shape a lens mask actually produces.
		for (uint32_t y : {0u, rows - 1})
			for (uint32_t x : {0u, cols - 1})
				m.skip[size_t(y) * cols + x] = 1;

		std::vector<vertex> got(max_vertices(cols, rows));
		const size_t n = emit(px, py, w, h, m, got.data(), got.size());
		check(n <= max_vertices(cols, rows), "stays inside the buffer");
		check(n > 0, "still draws something");

		// Every cell that was NOT masked must still be covered by some emitted
		// vertex pair: check its top-left source coordinate appears.
		uint32_t in_y = 0;
		bool all_present = true;
		for (uint32_t iy = 0; iy < rows; ++iy)
		{
			uint32_t in_x = 0;
			for (uint32_t ix = 0; ix < cols; ++ix)
			{
				if (not m.at(ix, iy))
				{
					bool found = false;
					for (size_t i = 0; i < n and not found; ++i)
						found = got[i].u == in_x and got[i].v == in_y;
					all_present = all_present and found;
				}
				in_x += px[ix];
			}
			in_y += py[iy];
		}
		check(all_present, "no unmasked cell was dropped");
	}

	std::printf("a fully masked grid emits nothing rather than something wrong\n");
	{
		const uint32_t cols = px.size(), rows = py.size();
		cell_mask m{.cols = cols, .rows = rows};
		m.skip.assign(size_t(cols) * rows, 1);
		std::vector<vertex> got(max_vertices(cols, rows));
		check(emit(px, py, w, h, m, got.data(), got.size()) == 0, "everything masked draws nothing");
	}

	std::printf("the lens cell mask never eats the overscan ring\n");
	{
		using namespace wivrn::view_geometry;
		// A symmetric 100 deg FOV, the shape the lens-mask tests use.
		const float a = float(100.0 * M_PI / 180.0 / 2.0);
		const fov_angles fov{-a, a, -a, a};

		const auto no_overscan = lens_cell_mask(fov, px, py, 0.0);
		const auto with_overscan = lens_cell_mask(fov, px, py, 0.05);
		check(no_overscan.cols == px.size() and no_overscan.rows == py.size(), "mask matches the grid shape");
		check(with_overscan.masked <= no_overscan.masked,
		      "overscan can only ever mask FEWER cells, never more");

		// Whatever is masked with overscan on must also be masked with it off: the
		// ring only ever adds picture that has to be kept.
		bool subset = true;
		for (uint32_t y = 0; y < no_overscan.rows and subset; ++y)
			for (uint32_t x = 0; x < no_overscan.cols and subset; ++x)
				if (with_overscan.at(x, y))
					subset = no_overscan.at(x, y);
		check(subset, "the overscan mask is a subset of the no-overscan mask");

		// And the corners are what a round lens actually removes.
		check(no_overscan.masked > 0, "a round region masks something at 100 deg");
		check(not no_overscan.at(no_overscan.cols / 2, no_overscan.rows / 2),
		      "the centre cell is never masked");
	}

	std::printf("a degenerate field of view masks nothing\n");
	{
		using namespace wivrn::view_geometry;
		const fov_angles zero{0, 0, 0, 0};
		check(lens_cell_mask(zero, px, py, 0.0).any() == false, "zero FOV yields an empty mask");
	}

	std::printf(failures ? "\n%d CHECK(S) FAILED\n" : "\nall checks passed\n", failures);
	return failures ? 1 : 0;
}
