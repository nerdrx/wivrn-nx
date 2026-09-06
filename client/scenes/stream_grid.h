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

// The reprojection pass's vertex grid, on its own so it can be tested without Vulkan.
//
// The pass draws one triangle strip per view over the foveation runs: a pair of vertices
// (top, bottom) at every column boundary of a row, then a repeated vertex to break the
// strip before the next row. That shape is unchanged here. What is added is the ability to
// leave out cells the lens can never show, which is the pass's largest remaining saving --
// it costs 6.37 ms of GPU per iteration and 278 ms/s on a Pico 4, and the corners of the
// image are outside the optics.
//
// The safety property this file exists to make testable: **a mask that is empty, wrong in
// the conservative direction, or absent must produce exactly the strip the pass drew
// before.** Cropping the picture is far worse than failing to save GPU, so every unknown
// resolves to "draw it".

#include <cstddef>
#include <cstdint>
#include "utils/view_geometry.h"

#include <cmath>
#include <cstdlib>
#include <vector>

namespace wivrn::stream_grid
{

// Layout-compatible with stream_defoveator::vertex (two alignas(8) glm 2-vectors), which
// static_asserts it. Note the declarators are separate on purpose: `alignas(8) float px,
// py;` aligns EACH of them to 8 and makes the struct 32 bytes rather than 16.
struct vertex
{
	alignas(8) float px;
	float py;
	alignas(8) uint32_t u;
	uint32_t v;
};

// Which cells of the foveation grid need not be drawn. Indexed [row * cols + col] over the
// grid's own cells -- one per (x run, y run) pair -- NOT over codec tiles: the caller
// translates from whatever geometry it has into this.
//
// Default-constructed means "nothing is skippable", which is the behaviour of the pass
// before any of this existed, and is what every failure path here produces.
struct cell_mask
{
	uint32_t cols = 0;
	uint32_t rows = 0;
	std::vector<uint8_t> skip;
	uint32_t masked = 0;

	bool any() const
	{
		return cols and rows and not skip.empty();
	}

	bool at(uint32_t x, uint32_t y) const
	{
		if (x >= cols or y >= rows)
			return false; // out of range is never skippable
		const size_t i = size_t(y) * cols + x;
		return i < skip.size() and skip[i];
	}

	// Is this whole row skippable? A fully skipped row emits nothing at all.
	bool row_empty(uint32_t y) const
	{
		if (not any() or y >= rows)
			return false;
		for (uint32_t x = 0; x < cols; ++x)
			if (not at(x, y))
				return false;
		return true;
	}
};

// Vertices the UNMASKED strip writes. This is the count the pass used before any of this
// and the count the buffer was always sized for.
//
//   0 2 4
//   1 3 5 5*   -- one such line per y run, plus a break
inline size_t unmasked_vertices(size_t nx, size_t ny)
{
	return (2 * (nx + 1) + 1) * ny;
}

// Upper bound once cells may be skipped, which is BIGGER than the unmasked count, not
// smaller. Getting this wrong is how the first version of this file overran its buffer:
// resuming a strip after a gap repeats a vertex, so an alternating skip/draw row writes
// more vertices than a solid one, not fewer. Worst case per cell is two for the cell, one
// to reopen after a gap and one to close before the next, plus the row's own tail.
inline size_t max_vertices(size_t nx, size_t ny)
{
	return (4 * (nx + 1) + 3) * ny;
}

// Emit the strip for one view. Returns the number of vertices written, which is what the
// draw call must use. `out` must have room for max_vertices(px.size(), py.size()).
//
// `mask` may be empty, in which case this writes exactly the strip the pass has always
// drawn, vertex for vertex.
inline size_t emit(const std::vector<uint16_t> & px,
                   const std::vector<uint16_t> & py,
                   int out_width,
                   int out_height,
                   const cell_mask & mask,
                   vertex * out,
                   size_t capacity)
{
	// Not enough room to skip safely? Draw the whole grid. Truncating a strip would
	// cut the picture off mid-row, which is the one outcome this must never produce.
	if (capacity < max_vertices(px.size(), py.size()) and
	    capacity >= unmasked_vertices(px.size(), py.size()))
		return emit(px, py, out_width, out_height, {}, out, capacity);

	const int n_ratio_x = (int(px.size()) - 1) / 2;
	const int n_ratio_y = (int(py.size()) - 1) / 2;
	const float out_pixel_x = 2.f / float(out_width);
	const float out_pixel_y = 2.f / float(out_height);

	// A mask whose shape does not match this grid describes a different grid, so it is
	// not usable and nothing is skipped. This is the check that makes a stale mask --
	// the foveation changed and the mask did not -- harmless rather than a crop.
	const bool usable = mask.any() and mask.cols == px.size() and mask.rows == py.size();

	vertex * w = out;
	uint32_t in_y = 0;
	float pos_y = -0.5f * float(out_height);

	for (size_t iy = 0; iy < py.size(); ++iy)
	{
		const int n_out_y = int(py[iy]);
		const int ratio_y = std::abs(n_ratio_y - int(iy)) + 1;

		// A row nothing is drawn in emits no vertices at all -- not even a break,
		// because there is no strip in progress to break.
		if (usable and mask.row_empty(uint32_t(iy)))
		{
			in_y += uint32_t(n_out_y);
			pos_y += float(n_out_y * ratio_y);
			continue;
		}

		uint32_t in_x = 0;
		float pos_x = -0.5f * float(out_width);
		bool strip_open = false;
		// A gap that needs closing only exists once something has been drawn in THIS
		// row. At the start of a row the previous row's tail has already broken the
		// strip, so reopening there would emit a vertex the unmasked grid does not --
		// which is exactly how the first version stopped being identical with no mask.
		bool drawn_in_row = false;
		bool gap_since_draw = false;

		for (size_t ix = 0; ix < px.size(); ++ix)
		{
			const int n_out_x = int(px[ix]);
			const int ratio_x = std::abs(n_ratio_x - int(ix)) + 1;
			const bool skip = usable and mask.at(uint32_t(ix), uint32_t(iy));

			if (not skip)
			{
				if (drawn_in_row and gap_since_draw)
					*w++ = {pos_x * out_pixel_x, pos_y * out_pixel_y, in_x, in_y};

				*w++ = {pos_x * out_pixel_x, pos_y * out_pixel_y, in_x, in_y};
				*w++ = {pos_x * out_pixel_x,
				        (pos_y + float(n_out_y * ratio_y)) * out_pixel_y,
				        in_x,
				        in_y + uint32_t(n_out_y)};
				strip_open = true;
				drawn_in_row = true;
				gap_since_draw = false;
			}
			else
			{
				if (strip_open)
				{
					// Close the run on the right edge of the last drawn cell.
					*w++ = {pos_x * out_pixel_x, pos_y * out_pixel_y, in_x, in_y};
					strip_open = false;
				}
				gap_since_draw = true;
			}

			in_x += uint32_t(n_out_x);
			pos_x += float(n_out_x * ratio_x);
		}

		// The row's tail, in the order the unmasked grid has always written it: the
		// right edge at THIS row's top, then the same corner at the NEXT row's top
		// twice, which is what breaks the strip. The advance happens between them, so
		// it cannot be hoisted out of this block.
		if (strip_open)
			*w++ = {pos_x * out_pixel_x, pos_y * out_pixel_y, in_x, in_y};

		in_y += uint32_t(n_out_y);
		pos_y += float(n_out_y * ratio_y);

		if (strip_open)
		{
			*w++ = {pos_x * out_pixel_x, pos_y * out_pixel_y, in_x, in_y};
			*w++ = {pos_x * out_pixel_x, pos_y * out_pixel_y, in_x, in_y};
		}
	}


	return size_t(w - out);
}

// Which cells of this view's grid the optics can never show.
//
// One `region_covers` test per grid cell, against the same ellipse the server's own lens
// mask uses -- so the two agree by construction rather than by both being careful.
//
// `overscan` MUST be the margin this session encodes. The overscan ring is off the panel
// at the pose the frame was rendered for and on it a few milliseconds later at the pose it
// is displayed at: that is the entire point of it, and a mask that treated it as unseen
// would delete the pixels edge bleed exists to provide. Passing it here grows the ellipse
// so the ring is never skippable.
//
// Every failure returns an empty mask, which `emit()` treats as "draw everything".
inline cell_mask lens_cell_mask(const view_geometry::fov_angles & fov,
                                const std::vector<uint16_t> & px,
                                const std::vector<uint16_t> & py,
                                double overscan)
{
	cell_mask m;
	if (px.empty() or py.empty())
		return m;

	// Source extent each axis covers, which is what the runs are expressed against.
	const auto extent = [](const std::vector<uint16_t> & p) {
		uint32_t n = 0;
		for (auto v: p)
			n += v;
		return n;
	};
	const double sw = extent(px), sh = extent(py);
	if (not(sw > 0) or not(sh > 0))
		return m;

	const auto r = view_geometry::fov_bounds(fov);
	const auto e = view_geometry::visible_region(fov, overscan);
	if (not(e.rx > 0) or not(e.ry > 0))
		return m; // a degenerate region masks nothing

	m.cols = uint32_t(px.size());
	m.rows = uint32_t(py.size());
	m.skip.assign(size_t(m.cols) * m.rows, 0);

	uint32_t in_y = 0;
	for (uint32_t iy = 0; iy < m.rows; ++iy)
	{
		const double v0 = double(in_y) / sh, v1 = double(in_y + py[iy]) / sh;
		uint32_t in_x = 0;
		for (uint32_t ix = 0; ix < m.cols; ++ix)
		{
			const double u0 = double(in_x) / sw, u1 = double(in_x + px[ix]) / sw;
			const view_geometry::tan_rect t{
			        r.x0 + u0 * (r.x1 - r.x0),
			        r.x0 + u1 * (r.x1 - r.x0),
			        r.y0 + v0 * (r.y1 - r.y0),
			        r.y0 + v1 * (r.y1 - r.y0),
			};
			// region_covers answers "does any part of this cell fall inside", so
			// a cell is skippable exactly when it does not. A cell straddling the
			// boundary answers yes and is drawn, which is the safe direction.
			if (not view_geometry::region_covers(e, t))
			{
				m.skip[size_t(iy) * m.cols + ix] = 1;
				++m.masked;
			}
			in_x += px[ix];
		}
		in_y += py[iy];
	}

	return m;
}

} // namespace wivrn::stream_grid
