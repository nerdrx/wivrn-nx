/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "wivrn_packets.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wivrn
{

// A whole motion field. It does not fit in a datagram, so it travels as several
// to_headset::motion_field chunks; this is what one looks like on either side of that
// cutting up.
struct motion_field_data
{
	// Video frame the field starts from
	uint64_t frame_idx = uint64_t(-1);
	// Interval it spans, in the headset time referential, strictly positive
	XrTime span_ns = 0;
	// Cells per eye
	uint16_t width = 0;
	uint16_t height = 0;
	// Longest displacement in the field, as a fraction of the eye image
	float scale = 0;
	// Two int8 components (x, y) per cell, row major, left eye then right eye:
	// index = ((view * height + j) * width + i) * 2
	std::vector<int8_t> vectors;

	// Number of int8 values a complete field of this size holds
	size_t value_count() const
	{
		return size_t(width) * height * 2 * 2;
	}
};

// Cuts a whole field into chunks of whole grid rows of one eye, each small enough for
// one datagram. Every chunk repeats the header, so they are independent of each other
// and of their order. Returns nothing for a field that has no cells.
inline std::vector<to_headset::motion_field> split_motion_field(const motion_field_data & field)
{
	std::vector<to_headset::motion_field> chunks;

	if (field.width == 0 or field.height == 0 or field.vectors.size() != field.value_count())
		return chunks;

	const size_t row_values = size_t(field.width) * 2;
	const uint16_t step = to_headset::motion_field::rows_per_chunk(field.width);

	for (uint8_t view = 0; view < 2; ++view)
	{
		for (uint16_t row = 0; row < field.height; row += step)
		{
			const uint16_t rows = std::min<uint16_t>(step, field.height - row);
			const auto first = field.vectors.begin() +
			                   ptrdiff_t((size_t(view) * field.height + row) * row_values);

			chunks.push_back({
			        .frame_idx = field.frame_idx,
			        .span_ns = field.span_ns,
			        .width = field.width,
			        .height = field.height,
			        .scale = field.scale,
			        .view = view,
			        .row_offset = row,
			        .row_count = rows,
			        .vectors = std::vector<int8_t>(first, first + ptrdiff_t(size_t(rows) * row_values)),
			});
		}
	}

	return chunks;
}

// Puts the chunks of a field back together. Only the newest field is worth anything —
// the consumer drops one that does not name the frame it is displaying — so a chunk of
// a frame older than the one being assembled is discarded, and a chunk of a newer one
// throws away whatever was incomplete.
class motion_field_assembler
{
	motion_field_data current;
	// Whether each of the height * 2 rows has arrived, and how many have not
	std::vector<bool> rows;
	size_t missing = 0;
	bool started = false;

public:
	// Whether field() holds a field every row of which has arrived
	bool complete() const
	{
		return started and missing == 0;
	}

	// Only meaningful once complete() is true
	const motion_field_data & field() const
	{
		return current;
	}

	void add(const to_headset::motion_field & chunk)
	{
		// Do not trust the sender's arithmetic
		if (chunk.width == 0 or chunk.height == 0 or chunk.row_count == 0 or chunk.view >= 2)
			return;
		if (size_t(chunk.row_offset) + chunk.row_count > chunk.height)
			return;
		if (chunk.vectors.size() != size_t(chunk.row_count) * chunk.width * 2)
			return;

		const bool same_field = started and current.frame_idx == chunk.frame_idx and
		                        current.width == chunk.width and current.height == chunk.height;

		if (not same_field)
		{
			// A straggler from a field that has already been superseded
			if (started and chunk.frame_idx < current.frame_idx)
				return;

			current.frame_idx = chunk.frame_idx;
			current.span_ns = chunk.span_ns;
			current.width = chunk.width;
			current.height = chunk.height;
			current.scale = chunk.scale;
			current.vectors.assign(current.value_count(), 0);
			rows.assign(size_t(chunk.height) * 2, false);
			missing = rows.size();
			started = true;
		}

		const size_t row_values = size_t(current.width) * 2;
		for (uint16_t j = 0; j < chunk.row_count; ++j)
		{
			const size_t row = size_t(chunk.view) * current.height + chunk.row_offset + j;
			std::copy_n(chunk.vectors.begin() + ptrdiff_t(size_t(j) * row_values),
			            row_values,
			            current.vectors.begin() + ptrdiff_t(row * row_values));
			if (not rows[row])
			{
				rows[row] = true;
				--missing;
			}
		}
	}
};

} // namespace wivrn
