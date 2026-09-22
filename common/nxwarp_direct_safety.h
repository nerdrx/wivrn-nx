// Independent safety prefix for NX direct streams (NXDB versions 5/6).
#pragma once
#include "nxwarp_direct.h"
#include <algorithm>

namespace wivrn::nxwarp_direct
{
constexpr uint32_t safety_magic = 0x5344584e;
constexpr size_t safety_header_bytes = 32;
struct safety_header
{
	layout low;
	uint32_t safety_bytes, detail_bytes;
	size_t prefix_bytes() const { return safety_header_bytes + size_t(safety_bytes); }
	size_t total_bytes() const { return prefix_bytes() + detail_bytes; }
};
inline std::optional<safety_header> parse_safety_header(layout full, std::span<const uint8_t> b)
{
	if (!full.valid() || b.size() < safety_header_bytes || read32(b, 0) != safety_magic ||
	    read32(b, 4) != 1 || read32(b, 28) != 0)
		return {};
	safety_header h{{read32(b, 8), read32(b, 12), read32(b, 16)}, read32(b, 20), read32(b, 24)};
	if (!h.low.valid() || h.low.native_center || h.low.eyes != full.eyes || h.low.width > full.width || h.low.height > full.height ||
	    h.safety_bytes < frame_header_bytes || h.safety_bytes > h.low.max_frame_bytes() ||
	    h.detail_bytes < frame_header_bytes || h.detail_bytes > full.max_frame_bytes())
		return {};
	return h;
}

// Recover ONLY a contiguous, independently complete safety prefix. Holes in detail
// are allowed; holes/short chunks in the safety prefix are never concealed.
inline std::vector<uint8_t> recover_safety_prefix(layout full,
        std::span<const std::vector<uint8_t>> slots, size_t chunk)
{
	if (chunk < 4 + safety_header_bytes || slots.empty() || slots[0].size() < 4 + safety_header_bytes)
		return {};
	const auto first = std::span<const uint8_t>(slots[0]);
	const auto h = parse_safety_header(full, first.subspan(4));
	if (!h || read32(first, 0) != h->total_bytes()) return {};
	const size_t required = 4 + h->prefix_bytes();
	const size_t wire_bytes = 4 + h->total_bytes();
	const size_t count = (required + chunk - 1) / chunk;
	if (count > slots.size()) return {};
	for (size_t i = 0; i < count; ++i)
		if (slots[i].size() != std::min(chunk, wire_bytes - i * chunk)) return {};
	std::vector<uint8_t> out;
	out.reserve(h->prefix_bytes());
	for (size_t i = 0; i < count; ++i)
	{
		const size_t skip = i == 0 ? 4 : 0;
		const size_t take = std::min(slots[i].size() - skip, h->prefix_bytes() - out.size());
		out.insert(out.end(), slots[i].begin() + skip, slots[i].begin() + skip + take);
	}
	return out;
}
} // namespace wivrn::nxwarp_direct
