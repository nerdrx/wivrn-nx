#include "scenes/atlas_tile_grid.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace wivrn::atlas_tile_grid;

static float triangle_area(const vertex & a, const vertex & b, const vertex & c)
{
	return std::abs((b.px - a.px) * (c.py - a.py) - (b.py - a.py) * (c.px - a.px)) * 0.5f;
}

int main()
{
	// Independent checks for the asymmetric foveation mapping (ratios 2,1,2).
	const std::vector<uint16_t> px{3, 5, 2};
	const std::vector<uint16_t> py{4, 1, 6};
	assert(std::abs(map_axis(px, 0) + 1.f) < 1e-6f);
	assert(std::abs(map_axis(px, 3) - (-0.2f)) < 1e-6f);
	assert(std::abs(map_axis(px, 8) - (11.f / 15.f * 2.f - 1.f)) < 1e-6f);
	assert(std::abs(map_axis(px, 10) - 1.f) < 1e-6f);
	assert(std::abs(map_axis(py, 4) - (16.f / 21.f - 1.f)) < 1e-6f);
	assert(std::abs(map_axis(py, 5) - (18.f / 21.f - 1.f)) < 1e-6f);

	const size_t needed = required_vertices(px, py);
	assert(needed == 6 * 3 * 3);
	std::vector<vertex> vertices(needed);
	assert(emit(px, py, vertices.data(), vertices.size()) == needed);

	float area = 0;
	for (size_t i = 0; i < vertices.size(); i += 6)
	{
		area += triangle_area(vertices[i], vertices[i + 1], vertices[i + 2]);
		area += triangle_area(vertices[i + 3], vertices[i + 4], vertices[i + 5]);
	}
	assert(std::abs(area - 4.f) < 1e-5f);

	std::vector<vertex> untouched(needed, vertex{17.f, 19.f, 23, 29, 31});
	assert(emit(px, py, untouched.data(), untouched.size() - 1) == 0);
	for (const auto & v: untouched)
		assert(v.px == 17.f and v.py == 19.f and v.u == 23 and v.v == 29 and v.tile == 31);

	// All six vertices of each independent triangle pair carry one tile number, and
	// each emitted cell stays within one source tile.
	for (size_t i = 0; i < vertices.size(); i += 6)
	{
		for (size_t j = 1; j < 6; ++j)
			assert(vertices[i + j].tile == vertices[i].tile);
		const uint32_t min_u = std::min({vertices[i].u, vertices[i + 1].u, vertices[i + 2].u});
		const uint32_t max_u = std::max({vertices[i].u, vertices[i + 1].u, vertices[i + 2].u});
		const uint32_t min_v = std::min({vertices[i].v, vertices[i + 1].v, vertices[i + 2].v});
		const uint32_t max_v = std::max({vertices[i].v, vertices[i + 1].v, vertices[i + 2].v});
		assert(max_u - min_u <= 64 and max_v - min_v <= 64);
		const uint32_t centroid_u = (vertices[i].u + vertices[i + 1].u + vertices[i + 2].u) / 3;
		const uint32_t centroid_v = (vertices[i].v + vertices[i + 1].v + vertices[i + 2].v) / 3;
		assert(vertices[i].tile == centroid_v / 64 + centroid_u / 64);
		for (size_t j = 0; j < 6; ++j)
		{
			assert(vertices[i + j].u <= 10 and vertices[i + j].v <= 11);
			assert(vertices[i + j].px >= -1.f and vertices[i + j].px <= 1.f);
			assert(vertices[i + j].py >= -1.f and vertices[i + j].py <= 1.f);
		}
	}
	assert(vertices[0].u == 0 and vertices[0].v == 0);
	assert(vertices[0].tile == 0);

	const std::vector<uint16_t> tiled_x{48, 96, 32};
	const std::vector<uint16_t> tiled_y{80, 40};
	const size_t tiled_needed = required_vertices(tiled_x, tiled_y);
	std::vector<vertex> tiled(tiled_needed);
	assert(emit(tiled_x, tiled_y, tiled.data(), tiled.size()) == tiled_needed);
	for (size_t i = 0; i < tiled.size(); i += 6)
	{
		const uint32_t min_u = std::min({tiled[i].u, tiled[i + 1].u, tiled[i + 2].u});
		const uint32_t max_u = std::max({tiled[i].u, tiled[i + 1].u, tiled[i + 2].u});
		const uint32_t min_v = std::min({tiled[i].v, tiled[i + 1].v, tiled[i + 2].v});
		const uint32_t max_v = std::max({tiled[i].v, tiled[i + 1].v, tiled[i + 2].v});
		assert(max_u - min_u <= 64 and max_v - min_v <= 64);
		const uint32_t cu = (tiled[i].u + tiled[i + 1].u + tiled[i + 2].u) / 3;
		const uint32_t cv = (tiled[i].v + tiled[i + 1].v + tiled[i + 2].v) / 3;
		assert(tiled[i].tile == (cv / 64) * 3 + cu / 64);
	}

	// Native dimensions and a clipped edge both retain complete tile coverage.
	const std::vector<uint16_t> native{2176};
	const std::vector<uint16_t> clipped{2173};
	assert(required_vertices(native, native) == 6 * 34 * 34);
	assert(required_vertices(clipped, clipped) == 6 * 34 * 34);
	const std::vector<uint16_t> display_extent{2160};
	assert(required_vertices(display_extent, display_extent) == required_vertices(native, native));
	std::vector<vertex> display(required_vertices(display_extent, display_extent));
	assert(emit(display_extent, display_extent, display.data(), display.size()) == display.size());
	for (const auto & v: display)
		assert(v.u <= 2160 and v.v <= 2160);
	std::cout << "atlas_tile_grid_test: pass\n";
}
