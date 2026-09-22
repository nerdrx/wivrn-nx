#pragma once

#include "nxwarp_direct.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace wivrn::nxwarp_direct
{
struct partial_recovery_result
{
	std::vector<uint8_t> unit;
	uint32_t fresh_tiles = 0;
	uint32_t retained_tiles = 0;
};

// Recover a frame whose transport chunks are fixed-size. `previous` is a validated
// frame unit (the 16-byte NXDF header, without the transport length prefix).
inline std::optional<partial_recovery_result> recover_partial(
	layout l, std::span<const std::vector<uint8_t>> slots, size_t chunk_bytes,
	std::span<const uint8_t> previous, uint32_t max_retained_tiles = UINT32_MAX)
{
	if (!l.valid() || slots.empty() || chunk_bytes < 4 || previous.empty())
		return {};
	auto old = parse_frame(l, previous);
	if (!old)
		return {};
	if (slots.front().size() < 4 || slots.front().size() > chunk_bytes)
		return {};
	const uint32_t declared = read32(slots.front(), 0);
	if (declared < frame_header_bytes || declared > l.max_frame_bytes())
		return {};
	const size_t wire_bytes = size_t(declared) + 4;
	const size_t count = wire_bytes / chunk_bytes + (wire_bytes % chunk_bytes != 0);
	if (count == 0 || count > slots.size())
		return {};
	for (size_t i = 0; i < count; ++i)
	{
		const size_t expected = std::min(chunk_bytes, wire_bytes - i * chunk_bytes);
		if (!slots[i].empty() && slots[i].size() != expected)
			return {};
	}
	for (size_t i = count; i < slots.size(); ++i)
		if (!slots[i].empty())
			return {};

	// Read only bytes whose complete fixed chunks arrived. This keeps holes explicit.
	auto available = [&](size_t offset, size_t bytes) {
		if (offset > wire_bytes || bytes > wire_bytes - offset)
			return false;
		if (!bytes)
			return true;
		const size_t first = offset / chunk_bytes, last = (offset + bytes - 1) / chunk_bytes;
		for (size_t i = first; i <= last; ++i)
			if (slots[i].empty())
				return false;
		return true;
	};
	auto byte_at = [&](size_t offset) -> uint8_t {
		const size_t i = offset / chunk_bytes;
		return slots[i][offset % chunk_bytes];
	};
	auto word_at = [&](size_t offset) {
		return uint32_t(byte_at(offset)) | (uint32_t(byte_at(offset + 1)) << 8) |
		       (uint32_t(byte_at(offset + 2)) << 16) | (uint32_t(byte_at(offset + 3)) << 24);
	};
	const size_t table_bytes = frame_header_bytes + size_t(l.tile_count()) * 4;
	if (!available(4, table_bytes))
		return {};
	if (word_at(4) != frame_magic || word_at(8) != version ||
		word_at(12) != l.tile_count())
		return {};
	const uint32_t words = word_at(16);
	if (words > l.max_block_words() || words % 5 ||
		declared != frame_header_bytes + size_t(l.tile_count() + words) * 4)
		return {};

	struct tile { uint32_t descriptor; uint32_t count; bool fresh; };
	std::vector<tile> tiles;
	tiles.reserve(l.tile_count());
	for (uint32_t i = 0; i < l.tile_count(); ++i)
	{
		const uint32_t d = word_at(20 + size_t(i) * 4), mode = d >> 30;
		if (mode == 3)
		{
			if (d & 0x3f000000u)
				return {};
			tiles.push_back({d, 0, true});
			continue;
		}
		const uint32_t n = 80u >> (mode * 2), offset = d & 0x3fffffffu;
		if (offset % 5 || uint64_t(offset) + n > words)
			return {};
		const size_t block = 4 + table_bytes + size_t(offset) * 4;
		tiles.push_back({d, n, available(block, size_t(n) * 4)});
	}

	partial_recovery_result result;
	result.unit = frame_header(l.tile_count(), 0);
	result.unit.reserve(frame_header_bytes + size_t(l.tile_count()) * 4 + size_t(words) * 4);
	result.unit.resize(frame_header_bytes + size_t(l.tile_count()) * 4);
	uint32_t packed = 0;
	for (uint32_t i = 0; i < l.tile_count(); ++i)
	{
		uint32_t d = tiles[i].descriptor, n = tiles[i].count;
		const bool fresh = tiles[i].fresh;
		if (!fresh)
		{
			if (result.retained_tiles >= max_retained_tiles)
				return {};
			d = read32(old->descriptors, size_t(i) * 4);
			const uint32_t mode = d >> 30;
			n = mode == 3 ? 0 : 80u >> (mode * 2);
			++result.retained_tiles;
		}
		else
			++result.fresh_tiles;
		if (n)
		{
			const uint32_t source = d & 0x3fffffffu;
			const size_t bytes = size_t(n) * 4;
			result.unit.resize(result.unit.size() + bytes);
			uint8_t * out = result.unit.data() + result.unit.size() - bytes;
			const uint8_t * in = fresh ? nullptr : old->blocks.data() + size_t(source) * 4;
			if (fresh)
			{
				const size_t block = 4 + table_bytes + size_t(source) * 4;
				size_t copied = 0;
				while (copied < bytes)
				{
					const size_t wire_offset = block + copied;
					const size_t in_chunk = wire_offset % chunk_bytes;
					const size_t take = std::min(bytes - copied, chunk_bytes - in_chunk);
					std::memcpy(out + copied, slots[wire_offset / chunk_bytes].data() + in_chunk, take);
					copied += take;
				}
			}
			else
				std::memcpy(out, in, bytes);
			d = (d & 0xc0000000u) | packed;
			packed += n;
		}
		result.unit[frame_header_bytes + size_t(i) * 4 + 0] = uint8_t(d);
		result.unit[frame_header_bytes + size_t(i) * 4 + 1] = uint8_t(d >> 8);
		result.unit[frame_header_bytes + size_t(i) * 4 + 2] = uint8_t(d >> 16);
		result.unit[frame_header_bytes + size_t(i) * 4 + 3] = uint8_t(d >> 24);
	}
	result.unit.resize(frame_header_bytes + size_t(l.tile_count()) * 4 + size_t(packed) * 4);
	result.unit[12] = uint8_t(packed); result.unit[13] = uint8_t(packed >> 8);
	result.unit[14] = uint8_t(packed >> 16); result.unit[15] = uint8_t(packed >> 24);
	if (!result.fresh_tiles || !parse_frame(l, result.unit))
		return {};
	return result;
}
} // namespace wivrn::nxwarp_direct
