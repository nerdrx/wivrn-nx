// NX direct blocks v1: bounded, little-endian, independent-frame format.
#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
namespace wivrn::nxwarp_direct
{
constexpr uint32_t stream_magic = 0x4244584e, frame_magic = 0x4644584e, version = 1;
constexpr size_t stream_header_bytes = 32, frame_header_bytes = 16;
struct layout
{
	uint32_t width = 0, height = 0, eyes = 0; // width per eye
	bool valid() const
	{
		return width && height && width <= 4096 && height <= 4096 &&
		       (eyes == 1 || eyes == 2) && width % 32 == 0 && height % 32 == 0;
	}
	uint32_t tile_count() const
	{
		return (width / 32) * (height / 32) * eyes;
	}
	uint32_t max_block_words() const
	{
		return tile_count() * 80;
	}
	uint32_t max_frame_bytes() const
	{
		return 16 + 4 * (tile_count() + max_block_words());
	}
};
inline uint32_t read32(std::span<const uint8_t> b, size_t p)
{
	return uint32_t(b[p]) | (uint32_t(b[p + 1]) << 8) | (uint32_t(b[p + 2]) << 16) | (uint32_t(b[p + 3]) << 24);
}
inline void append32(std::vector<uint8_t> & b, uint32_t v)
{
	for (unsigned i = 0; i < 4; i++)
		b.push_back(uint8_t(v >> (8 * i)));
}
inline bool is_stream(std::span<const uint8_t> b)
{
	return b.size() >= 4 && read32(b, 0) == stream_magic;
}
// Versions 1/2: raw; 3/4: optional LZ4 units. Even versions use trusted-LAN CRC.
// Packed NXDF frames remain v1. Older clients reject unsupported stream versions.
inline std::vector<uint8_t> stream_header(layout l, bool trusted_lan = false, bool lz4 = false)
{
	if (!l.valid())
		return {};
	std::vector<uint8_t> b;
	b.reserve(32);
	for (uint32_t v: {stream_magic, (lz4 ? 3u : 1u) + (trusted_lan ? 1u : 0u), l.width, l.height, l.eyes, l.tile_count(), l.max_block_words(), l.max_frame_bytes()})
		append32(b, v);
	return b;
}
inline std::optional<layout> parse_stream(std::span<const uint8_t> b)
{
	if (b.size() != 32 || !is_stream(b) || (read32(b, 4) < 1 || read32(b, 4) > 4))
		return {};
	layout l{read32(b, 8), read32(b, 12), read32(b, 16)};
	if (!l.valid() || read32(b, 20) != l.tile_count() || read32(b, 24) != l.max_block_words() || read32(b, 28) != l.max_frame_bytes())
		return {};
	return l;
}
struct frame_view
{
	std::span<const uint8_t> descriptors, blocks;
};
inline std::optional<frame_view> parse_frame(layout l, std::span<const uint8_t> b)
{
	if (!l.valid() || b.size() < 16 || b.size() > l.max_frame_bytes() || read32(b, 0) != frame_magic || read32(b, 4) != version)
		return {};
	uint32_t n = read32(b, 8), words = read32(b, 12);
	if (n != l.tile_count() || words > l.max_block_words() || words % 5 || b.size() != 16ull + 4ull * (n + words))
		return {};
	frame_view v{b.subspan(16, n * 4), b.subspan(16 + n * 4, words * 4)};
	for (uint32_t i = 0; i < n; i++)
	{
		uint32_t d = read32(v.descriptors, i * 4), mode = d >> 30;
		if (mode == 3)
		{
			if (d & 0x3f000000u)
				return {};
			continue;
		} // inline 0xRRGGBB
		uint32_t offset = d & 0x3fffffffu, count = 80u >> (mode * 2);
		if (offset % 5 || uint64_t(offset) + count > words)
			return {};
	}
	return v;
}
inline std::vector<uint8_t> frame_header(uint32_t descriptors, uint32_t words)
{
	std::vector<uint8_t> b;
	b.reserve(16);
	for (uint32_t v: {frame_magic, version, descriptors, words})
		append32(b, v);
	return b;
}
} // namespace wivrn::nxwarp_direct
