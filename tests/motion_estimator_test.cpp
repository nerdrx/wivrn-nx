// Motion smoothing block matcher test harness.
//
// The search in server/compositor/shaders/motion_estimate.comp cannot run without a
// GPU session, so this mirrors it on the CPU, statement for statement, and includes
// the same constants file the shader does. Feed it synthetic image pairs with a
// known displacement and check that the vectors that come out are the ones that went
// in.
//
// What it does *not* cover: the downsample pass, the Vulkan plumbing, and the shared
// memory reduction (the packed key makes the reduction order irrelevant, which is why
// it is packed that way).
//
// Build:
//   g++ -std=c++23 -O2 -o motion_estimator_test motion_estimator_test.cpp && ./motion_estimator_test

#include "../server/compositor/shaders/motion_constants.glsl.inc"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                                   \
	do                                                                            \
	{                                                                             \
		++checks;                                                             \
		if (not(cond))                                                        \
		{                                                                     \
			++failures;                                                   \
			std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                     \
	} while (0)

namespace
{

struct ivec2
{
	int x = 0;
	int y = 0;

	ivec2 operator+(ivec2 o) const
	{
		return {x + o.x, y + o.y};
	}
	ivec2 operator-(ivec2 o) const
	{
		return {x - o.x, y - o.y};
	}
	ivec2 operator*(int s) const
	{
		return {x * s, y * s};
	}
	bool operator==(const ivec2 &) const = default;
};

struct plane
{
	int width = 0;
	int height = 0;
	std::vector<uint8_t> texels;

	uint8_t at(int x, int y) const
	{
		x = std::clamp(x, 0, width - 1);
		y = std::clamp(y, 0, height - 1);
		return texels[size_t(y) * width + x];
	}
};

// One luma pyramid, level 0 first
using pyramid = std::vector<plane>;

/*
 *
 * Geometry, mirroring motion_estimator.cpp
 *
 */

uint32_t divide_and_round_up(uint32_t a, uint32_t b)
{
	return (a + b - 1) / b;
}

uint32_t level0_dimension(uint32_t x)
{
	const uint32_t coarsest = 1u << (MOTION_LEVELS - 1);
	uint32_t n = divide_and_round_up(std::max<uint32_t>(x, 1), 1u << MOTION_L0_SHIFT);
	return std::max(coarsest, divide_and_round_up(n, coarsest) * coarsest);
}

uint32_t grid_dimension(uint32_t x)
{
	return std::max<uint32_t>(1, (x + MOTION_BLOCK_PX / 2) / MOTION_BLOCK_PX);
}

/*
 *
 * The search, mirroring motion_estimate.comp
 *
 */

uint32_t sad(const pyramid & current, const pyramid & previous, int level, ivec2 c, ivec2 d)
{
	uint32_t cost = 0;
	for (int y = 0; y < MOTION_WINDOW; ++y)
	{
		for (int x = 0; x < MOTION_WINDOW; ++x)
		{
			ivec2 o{x - MOTION_WINDOW / 2, y - MOTION_WINDOW / 2};
			int a = current[level].at(c.x + o.x, c.y + o.y);
			int b = previous[level].at(c.x + o.x - d.x, c.y + o.y - d.y);
			cost += uint32_t(std::abs(a - b));
		}
	}
	return cost;
}

// The shader packs cost, vector length and candidate index into one key and takes
// the minimum, so that the order the work group reduces in cannot change the answer.
// Doing the same here is what makes the two comparable at all.
ivec2 search(const pyramid & current, const pyramid & previous, int level, ivec2 c, ivec2 base, int radius)
{
	const int n = 2 * radius + 1;
	const uint32_t count = uint32_t(n * n);

	uint32_t best = 0xFFFFFFFFu;
	for (uint32_t i = 0; i < count; ++i)
	{
		int dy = int(i) / n - radius;
		int dx = int(i) % n - radius;
		uint32_t cost = sad(current, previous, level, c, base + ivec2{dx, dy});
		uint32_t length = uint32_t(std::abs(dx) + std::abs(dy));

		if (cost >= (1u << 14))
		{
			++failures;
			std::printf("  FAIL cost %u overflows the key packing\n", cost);
			cost = (1u << 14) - 1;
		}

		best = std::min(best, (cost << 12) | (length << 8) | i);
	}

	int i = int(best & 0xFFu);
	return base + ivec2{i % n - radius, i / n - radius};
}

float subtexel(uint32_t a, uint32_t b, uint32_t c)
{
	float d = float(a) + float(c) - 2.f * float(b);
	if (d <= 0.f)
		return 0.f;
	return std::clamp(0.5f * (float(a) - float(c)) / d, -0.5f, 0.5f);
}

// Displacement of one cell, in level 0 texels
std::pair<float, float> estimate_cell(
        const pyramid & current,
        const pyramid & previous,
        ivec2 cell,
        ivec2 grid)
{
	const int l0_width = current[0].width;
	const int l0_height = current[0].height;

	float centre_x = (cell.x + 0.5f) / float(grid.x) * float(l0_width);
	float centre_y = (cell.y + 0.5f) / float(grid.y) * float(l0_height);

	ivec2 v{0, 0};
	for (int level = MOTION_LEVELS - 1; level >= 0; --level)
	{
		int radius = level == MOTION_LEVELS - 1 ? MOTION_RADIUS_COARSE : MOTION_RADIUS_FINE;
		ivec2 c{int(std::floor(centre_x)) >> level, int(std::floor(centre_y)) >> level};
		ivec2 base{
		        int(std::floor(v.x / float(1 << level) + 0.5f)),
		        int(std::floor(v.y / float(1 << level) + 0.5f))};
		v = search(current, previous, level, c, base, radius) * (1 << level);
	}

	ivec2 c0{int(std::floor(centre_x)), int(std::floor(centre_y))};
	const ivec2 offsets[5] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
	uint32_t cost[5];
	for (int i = 0; i < 5; ++i)
		cost[i] = sad(current, previous, 0, c0, v + offsets[i]);

	return {
	        v.x + subtexel(cost[1], cost[0], cost[2]),
	        v.y + subtexel(cost[3], cost[0], cost[4]),
	};
}

/*
 *
 * Synthetic images
 *
 */

// A full resolution luma image with enough non repeating detail at every scale for
// block matching to have a unique answer. Sums of incommensurable sinusoids plus a
// little noise: repeating content is what makes a block matcher pick the wrong
// candidate, so it is deliberately avoided here.
plane make_image(int width, int height, uint32_t seed)
{
	plane res{.width = width, .height = height};
	res.texels.resize(size_t(width) * height);

	std::mt19937 rng(seed);
	std::uniform_real_distribution<float> phase(0, 6.283185f);

	struct wave
	{
		float fx, fy, p, a;
	};
	std::vector<wave> waves;
	for (int i = 0; i < 24; ++i)
	{
		float f = 0.002f * std::pow(1.35f, float(i));
		float angle = phase(rng);
		waves.push_back({f * std::cos(angle), f * std::sin(angle), phase(rng), 1.f / (1.f + 0.25f * i)});
	}

	std::uniform_int_distribution<int> noise(-3, 3);
	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			float v = 0;
			float norm = 0;
			for (const auto & w: waves)
			{
				v += w.a * std::sin(w.fx * x + w.fy * y + w.p);
				norm += w.a;
			}
			int level = int(128 + 110 * v / norm) + noise(rng);
			res.texels[size_t(y) * width + x] = uint8_t(std::clamp(level, 0, 255));
		}
	}
	return res;
}

// Box average of the source over the footprint of one destination texel, which is
// what motion_downsample.comp computes with bilinear taps.
pyramid make_pyramid(const plane & source, int l0_width, int l0_height)
{
	pyramid res;
	for (int level = 0; level < MOTION_LEVELS; ++level)
	{
		plane p{
		        .width = std::max(l0_width >> level, 1),
		        .height = std::max(l0_height >> level, 1),
		};
		p.texels.resize(size_t(p.width) * p.height);

		float step_x = float(source.width) / float(p.width);
		float step_y = float(source.height) / float(p.height);

		for (int y = 0; y < p.height; ++y)
		{
			for (int x = 0; x < p.width; ++x)
			{
				int x0 = int(x * step_x);
				int x1 = std::max(x0 + 1, int((x + 1) * step_x));
				int y0 = int(y * step_y);
				int y1 = std::max(y0 + 1, int((y + 1) * step_y));

				uint32_t sum = 0;
				uint32_t n = 0;
				for (int sy = y0; sy < y1; ++sy)
					for (int sx = x0; sx < x1; ++sx, ++n)
						sum += source.at(sx, sy);

				p.texels[size_t(y) * p.width + x] = uint8_t((sum + n / 2) / std::max<uint32_t>(n, 1));
			}
		}
		res.push_back(std::move(p));
	}
	return res;
}

// The image shifted by (dx, dy) full resolution pixels: shifted(p) = source(p - d),
// which is the relation the estimator is meant to recover.
plane shift(const plane & source, int dx, int dy)
{
	plane res{.width = source.width, .height = source.height};
	res.texels.resize(source.texels.size());
	for (int y = 0; y < res.height; ++y)
		for (int x = 0; x < res.width; ++x)
			res.texels[size_t(y) * res.width + x] = source.at(x - dx, y - dy);
	return res;
}

// A per pixel shift chosen by a callback, so a single image can carry different
// motion in different places.
template <typename F>
plane shift_by_region(const plane & source, F && displacement)
{
	plane res{.width = source.width, .height = source.height};
	res.texels.resize(source.texels.size());
	for (int y = 0; y < res.height; ++y)
	{
		for (int x = 0; x < res.width; ++x)
		{
			auto [dx, dy] = displacement(x, y);
			res.texels[size_t(y) * res.width + x] = source.at(x - dx, y - dy);
		}
	}
	return res;
}

/*
 *
 * Tests
 *
 */

const int eye_width = 1728;
const int eye_height = 1824;
const int l0_width = int(level0_dimension(eye_width));
const int l0_height = int(level0_dimension(eye_height));
const int grid_width = int(grid_dimension(eye_width));
const int grid_height = int(grid_dimension(eye_height));
// Full resolution pixels per level 0 texel
const float pixels_per_texel = float(eye_width) / float(l0_width);

// Reach of the whole hierarchy, in full resolution pixels
const float search_reach = float(MOTION_RADIUS_COARSE) * (1 << (MOTION_LEVELS - 1)) * pixels_per_texel;
// Half the matching window at the coarsest level, in full resolution pixels
const float half_window = MOTION_WINDOW * (1 << (MOTION_LEVELS - 1)) * pixels_per_texel * 0.5f;

// A cell this close to an edge of the image sees content that moved in from outside
// it, which clamp to edge replaces with a smear of the border. There is no
// displacement to recover there and the estimator is not expected to invent one; the
// same is true of the real thing, and the headset stretches the edge anyway.
bool near_border(int i, int j, float margin)
{
	float x = (i + 0.5f) / grid_width * eye_width;
	float y = (j + 0.5f) / grid_height * eye_height;
	return x < margin or x > eye_width - margin or y < margin or y > eye_height - margin;
}

// All statistics are over the interior cells: the border ones have no answer to be
// right about, and there are enough of them at this grid size to swamp a percentile.
// The worst border error is reported separately, as information.
struct result
{
	float mean = 0;
	float p99 = 0;
	float worst = 0;
	float worst_border = 0;
	int cells = 0;
	int border = 0;
};

// Runs the estimator over the whole grid and reports the error, in full resolution
// pixels, against the displacement that was applied.
template <typename Expected>
result run(const plane & previous_image, const plane & current_image, Expected && expected)
{
	pyramid previous = make_pyramid(previous_image, l0_width, l0_height);
	pyramid current = make_pyramid(current_image, l0_width, l0_height);

	const float margin = search_reach + half_window;

	result res;
	double total = 0;
	std::vector<float> errors;
	for (int j = 0; j < grid_height; ++j)
	{
		for (int i = 0; i < grid_width; ++i)
		{
			auto [vx, vy] = estimate_cell(current, previous, {i, j}, {grid_width, grid_height});

			// Level 0 texels back to full resolution pixels
			float got_x = vx * pixels_per_texel;
			float got_y = vy * (float(eye_height) / float(l0_height));

			auto [want_x, want_y] = expected(i, j);
			float error = std::hypot(got_x - want_x, got_y - want_y);

			if (near_border(i, j, margin))
			{
				res.worst_border = std::max(res.worst_border, error);
				++res.border;
				continue;
			}

			errors.push_back(error);
			total += error;
			res.worst = std::max(res.worst, error);
			++res.cells;
		}
	}

	std::ranges::sort(errors);
	res.mean = float(total / std::max(res.cells, 1));
	res.p99 = errors.empty() ? 0 : errors[std::min<size_t>(errors.size() - 1, errors.size() * 99 / 100)];
	return res;
}

void test_uniform_shifts()
{
	std::printf("uniform shifts\n");
	plane base = make_image(eye_width, eye_height, 1234);

	std::printf("  search reach: %.0f px, cell: %.0f px, grid: %dx%d\n",
	            search_reach,
	            float(eye_width) / grid_width,
	            grid_width,
	            grid_height);

	for (auto [dx, dy]: {std::pair{0, 0}, {4, 0}, {0, -4}, {13, 7}, {-24, 18}, {40, -32}, {-64, -48}})
	{
		if (std::abs(dx) > search_reach or std::abs(dy) > search_reach)
			continue;

		auto res = run(base, shift(base, dx, dy), [dx, dy](int, int) { return std::pair{float(dx), float(dy)}; });
		std::printf("  (%4d, %4d): over %d cells mean %.2f px, p99 %.2f px, worst %.2f px; %d border cells, worst %.2f px\n",
		            dx,
		            dy,
		            res.cells,
		            res.mean,
		            res.p99,
		            res.worst,
		            res.border,
		            res.worst_border);

		// One level 0 texel is four full resolution pixels; the sub-texel fit
		// should bring the typical cell well inside that.
		CHECK(res.mean < pixels_per_texel);
		CHECK(res.p99 < 2 * pixels_per_texel);
		CHECK(res.worst < 3 * pixels_per_texel);
	}
}

void test_local_motion()
{
	std::printf("locality\n");
	plane base = make_image(eye_width, eye_height, 99);

	// Left half moves one way, right half the other. Cells that straddle the seam
	// see two motions at once and can legitimately land on either, so they are
	// excluded from the error.
	const int seam = eye_width / 2;
	auto displacement = [seam](int x, int) {
		return x < seam ? std::pair{20, 6} : std::pair{-16, -10};
	};

	pyramid previous = make_pyramid(base, l0_width, l0_height);
	pyramid current = make_pyramid(shift_by_region(base, displacement), l0_width, l0_height);

	float worst = 0;
	int counted = 0;
	int skipped = 0;
	for (int j = 0; j < grid_height; ++j)
	{
		for (int i = 0; i < grid_width; ++i)
		{
			// The window is wider than the cell, so a cell within half a window of
			// the seam sees both motions at once and may legitimately pick either
			float centre = (i + 0.5f) / grid_width * eye_width;
			if (std::abs(centre - seam) < half_window or near_border(i, j, search_reach + half_window))
			{
				++skipped;
				continue;
			}

			auto [vx, vy] = estimate_cell(current, previous, {i, j}, {grid_width, grid_height});
			auto [want_x, want_y] = displacement(int(centre), 0);
			float error = std::hypot(vx * pixels_per_texel - want_x,
			                         vy * (float(eye_height) / float(l0_height)) - want_y);
			worst = std::max(worst, error);
			++counted;
		}
	}

	std::printf("  worst %.2f px over %d cells (%d seam or border cells skipped)\n", worst, counted, skipped);
	CHECK(counted > 0);
	CHECK(worst < 3 * pixels_per_texel);
}

void test_out_of_range()
{
	std::printf("beyond the search range\n");
	plane base = make_image(eye_width, eye_height, 7);

	// Far more motion than the pyramid can follow. The estimator must not report
	// something absurd: whatever it picks is bounded by the search range, and that
	// is all this checks. The transmitted field is clamped again to
	// MOTION_MAX_DISPLACEMENT on top.
	pyramid previous = make_pyramid(base, l0_width, l0_height);
	pyramid current = make_pyramid(shift(base, 400, 300), l0_width, l0_height);

	const float reach_texels = float(MOTION_RADIUS_COARSE) * (1 << (MOTION_LEVELS - 1)) +
	                           float(MOTION_RADIUS_FINE) * ((1 << (MOTION_LEVELS - 1)) - 1) + 0.5f;

	float worst = 0;
	for (int j = 0; j < grid_height; ++j)
	{
		for (int i = 0; i < grid_width; ++i)
		{
			auto [vx, vy] = estimate_cell(current, previous, {i, j}, {grid_width, grid_height});
			worst = std::max({worst, std::abs(vx), std::abs(vy)});
		}
	}

	std::printf("  worst reported %.2f texels, search reach %.2f texels\n", worst, reach_texels);
	CHECK(worst <= reach_texels);
}

void test_flat_image()
{
	std::printf("featureless image\n");

	// Nothing to match: every candidate costs the same, and the packed key must then
	// pick the shortest vector, which is the only safe answer.
	plane flat{.width = eye_width, .height = eye_height};
	flat.texels.assign(size_t(eye_width) * eye_height, 128);

	pyramid previous = make_pyramid(flat, l0_width, l0_height);
	pyramid current = make_pyramid(flat, l0_width, l0_height);

	auto [vx, vy] = estimate_cell(current, previous, {grid_width / 2, grid_height / 2}, {grid_width, grid_height});
	std::printf("  reported (%.2f, %.2f) texels\n", vx, vy);
	CHECK(vx == 0);
	CHECK(vy == 0);
}

} // namespace

int main()
{
	std::printf("motion estimator, %d levels, level 0 at 1/%d, window %d, radii %d/%d\n\n",
	            MOTION_LEVELS,
	            1 << MOTION_L0_SHIFT,
	            MOTION_WINDOW,
	            MOTION_RADIUS_COARSE,
	            MOTION_RADIUS_FINE);

	// The packing in motion_estimate.comp only holds for these bounds
	static_assert(MOTION_WINDOW * MOTION_WINDOW * 255 < (1 << 14), "cost does not fit in the key");
	static_assert((2 * MOTION_RADIUS_COARSE + 1) * (2 * MOTION_RADIUS_COARSE + 1) <= 256,
	              "candidate index does not fit in the key");
	static_assert(2 * MOTION_RADIUS_COARSE < 16, "vector length does not fit in the key");

	test_uniform_shifts();
	std::printf("\n");
	test_local_motion();
	std::printf("\n");
	test_out_of_range();
	std::printf("\n");
	test_flat_image();

	std::printf("\n%d checks, %d failures\n", checks, failures);
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
