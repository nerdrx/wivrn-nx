// Lossless post-pass for direct frames: fold uniform detail tiles into mode 3.
#pragma once

#include "nxwarp_direct.h"

#include <cstdint>
#include <span>
#include <vector>

namespace wivrn::nxwarp_direct
{
inline std::span<const uint8_t> compact_flat(layout l, std::span<const uint8_t> raw, std::vector<uint8_t> & out)
{
	const auto parsed = parse_frame(l, raw);
	if (!parsed)
		return raw;

	std::vector<uint32_t> descriptors(l.tile_count());
	bool changed = false;
	for (uint32_t tile = 0; tile < l.tile_count(); ++tile)
	{
		const uint32_t descriptor = read32(parsed->descriptors, tile * 4);
		const uint32_t mode = descriptor >> 30;
		if (mode == 3)
		{
			descriptors[tile] = descriptor;
			continue;
		}
		const uint32_t blocks = 16u >> (2u * mode);
		const uint32_t offset = descriptor & 0x3fffffffu;
		const uint32_t words_per_block = 5;
		uint32_t first = 0;
		bool have_first = false, flat = true;
		for (uint32_t block = 0; block < blocks && flat; ++block)
		{
			const uint32_t base = (offset + block * words_per_block) * 4;
			const uint32_t endpoints = read32(parsed->blocks, base);
			const uint32_t p0 = endpoints & 0xffffu, p1 = endpoints >> 16;
			const uint32_t r0 = (((p0 >> 11) & 31u) << 3) | (((p0 >> 11) & 31u) >> 2);
			const uint32_t g0 = (((p0 >> 5) & 63u) << 2) | (((p0 >> 5) & 63u) >> 4);
			const uint32_t b0 = ((p0 & 31u) << 3) | ((p0 & 31u) >> 2);
			const uint32_t r1 = (((p1 >> 11) & 31u) << 3) | (((p1 >> 11) & 31u) >> 2);
			const uint32_t g1 = (((p1 >> 5) & 63u) << 2) | (((p1 >> 5) & 63u) >> 4);
			const uint32_t b1 = ((p1 & 31u) << 3) | ((p1 & 31u) >> 2);
			if (p0 == p1)
			{
				const uint32_t colour = (r0 << 16) | (g0 << 8) | b0;
				if (!have_first)
					first = colour, have_first = true;
				else if (colour != first)
					flat = false;
				continue;
			}
			uint32_t selectors[4];
			for (uint32_t i = 0; i < 4; ++i)
				selectors[i] = read32(parsed->blocks, base + (1 + i) * 4);
			const uint32_t selector = selectors[0] & 3u;
			bool uniform = selectors[1] == selectors[0] && selectors[2] == selectors[0] && selectors[3] == selectors[0];
			const uint32_t repeated = selector * 0x55555555u;
			uniform = uniform && selectors[0] == repeated;
			if (uniform)
			{
				const uint32_t r = ((3 - selector) * r0 + selector * r1 + 1) / 3;
				const uint32_t g = ((3 - selector) * g0 + selector * g1 + 1) / 3;
				const uint32_t b = ((3 - selector) * b0 + selector * b1 + 1) / 3;
				const uint32_t colour = (r << 16) | (g << 8) | b;
				if (!have_first)
					first = colour, have_first = true;
				else if (colour != first)
					flat = false;
				continue;
			}
			for (uint32_t pixel = 0; pixel < 64 && flat; ++pixel)
			{
				const uint32_t q = (selectors[pixel / 16] >> (2 * (pixel % 16))) & 3u;
				const uint32_t r = ((3 - q) * r0 + q * r1 + 1) / 3;
				const uint32_t g = ((3 - q) * g0 + q * g1 + 1) / 3;
				const uint32_t b = ((3 - q) * b0 + q * b1 + 1) / 3;
				const uint32_t colour = (r << 16) | (g << 8) | b;
				if (!have_first)
					first = colour, have_first = true;
				else if (colour != first)
					flat = false;
			}
		}
		if (flat)
			descriptors[tile] = 0xc0000000u | first, changed = true;
		else
			descriptors[tile] = descriptor;
	}
	if (!changed)
		return raw;

	uint32_t words = 0;
	for (uint32_t tile = 0; tile < l.tile_count(); ++tile)
	{
		const uint32_t original = read32(parsed->descriptors, tile * 4);
		if ((descriptors[tile] >> 30) == 3)
			continue;
		const uint32_t mode = original >> 30;
		const uint32_t count = 80u >> (mode * 2);
		const uint32_t offset = original & 0x3fffffffu;
		const uint32_t rewritten = words;
		descriptors[tile] = (mode << 30) | rewritten;
		words += count;
	}
	out = frame_header(l.tile_count(), words);
	out.reserve(16 + l.tile_count() * 4ull + words * 4ull);
	for (uint32_t descriptor: descriptors)
		append32(out, descriptor);
	for (uint32_t tile = 0; tile < l.tile_count(); ++tile)
	{
		const uint32_t original = read32(parsed->descriptors, tile * 4);
		if ((descriptors[tile] >> 30) == 3)
			continue;
		const uint32_t mode = original >> 30;
		const uint32_t count = 80u >> (mode * 2);
		const uint32_t offset = original & 0x3fffffffu;
		out.insert(out.end(), parsed->blocks.begin() + offset * 4, parsed->blocks.begin() + (offset + count) * 4);
	}
	return out;
}
} // namespace wivrn::nxwarp_direct
