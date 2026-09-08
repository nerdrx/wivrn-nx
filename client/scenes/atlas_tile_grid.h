/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

// CPU-side geometry for the native atlas consumer.  A cell is split wherever either
// the codec tile (64 source pixels) or the foveation run changes.  The fragment shader
// receives the tile number as a flat vertex attribute; the projective quotient remains
// in the fragment shader, so no interpolation is performed after dividing by q.z.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace wivrn::atlas_tile_grid
{

struct vertex
{
	float px;
	float py;
	uint32_t u;
	uint32_t v;
	uint32_t tile;
};

inline std::vector<uint32_t> axis_breaks(const std::vector<uint16_t> & runs)
{
	std::vector<uint32_t> result{0};
	uint32_t total = 0;
	for (uint16_t run: runs)
	{
		for (uint32_t boundary = ((total / 64) + 1) * 64; boundary < total + run; boundary += 64)
			result.push_back(boundary);
		total += run;
		result.push_back(total);
	}
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}

inline uint32_t extent(const std::vector<uint16_t> & runs)
{
	uint32_t result = 0;
	for (uint16_t run: runs)
		result += run;
	return result;
}

inline float map_axis(const std::vector<uint16_t> & runs, uint32_t source)
{
	if (runs.empty())
		return -1.f;
	const uint32_t source_extent = extent(runs);
	if (source_extent == 0)
		return -1.f;
	const int centre = (int(runs.size()) - 1) / 2;
	uint32_t in = 0;
	uint32_t out = 0;
	uint32_t out_extent = 0;
	for (size_t i = 0; i < runs.size(); ++i)
		out_extent += uint32_t(runs[i]) * (std::abs(centre - int(i)) + 1);
	if (out_extent == 0)
		return -1.f;
	for (size_t i = 0; i < runs.size(); ++i)
	{
		const uint32_t next = in + runs[i];
		const uint32_t clamped = std::min(std::max(source, in), next);
		const uint32_t ratio = std::abs(centre - int(i)) + 1;
		out += (clamped - in) * ratio;
		if (source <= next)
			break;
		in = next;
	}
	if (source > source_extent)
		out = out_extent;
	return -1.f + 2.f * float(out) / float(out_extent);
}

inline size_t required_vertices(const std::vector<uint16_t> & px, const std::vector<uint16_t> & py)
{
	const auto xb = axis_breaks(px);
	const auto yb = axis_breaks(py);
	return xb.size() > 1 and yb.size() > 1 ? (xb.size() - 1) * (yb.size() - 1) * 6 : 0;
}

inline size_t emit(const std::vector<uint16_t> & px,
                   const std::vector<uint16_t> & py,
                   vertex * out,
                   size_t capacity)
{
	const auto xb = axis_breaks(px);
	const auto yb = axis_breaks(py);
	const size_t needed = xb.size() > 1 and yb.size() > 1 ? (xb.size() - 1) * (yb.size() - 1) * 6 : 0;
	if (needed == 0 or capacity < needed)
		return 0;
	const uint32_t tile_columns = (extent(px) + 63) / 64;
	vertex * w = out;
	for (size_t y = 0; y + 1 < yb.size(); ++y)
	{
		for (size_t x = 0; x + 1 < xb.size(); ++x)
		{
			const uint32_t x0 = xb[x], x1 = xb[x + 1], y0 = yb[y], y1 = yb[y + 1];
			const uint32_t tile = (y0 / 64) * tile_columns + x0 / 64;
			const vertex tl{map_axis(px, x0), map_axis(py, y0), x0, y0, tile};
			const vertex tr{map_axis(px, x1), map_axis(py, y0), x1, y0, tile};
			const vertex bl{map_axis(px, x0), map_axis(py, y1), x0, y1, tile};
			const vertex br{map_axis(px, x1), map_axis(py, y1), x1, y1, tile};
			*w++ = tl;
			*w++ = bl;
			*w++ = br;
			*w++ = tl;
			*w++ = br;
			*w++ = tr;
		}
	}
	return needed;
}

} // namespace wivrn::atlas_tile_grid
