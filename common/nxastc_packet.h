#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>

namespace wivrn::nxastc_packet
{
inline constexpr size_t header_size = 24;
enum class compression : uint8_t
{
	none = 0,
	lz4 = 1,
	zstd = 2,
};

struct packet_header
{
	uint32_t width, height, raw_bytes, payload_bytes;
	compression encoding;
};

inline constexpr uint64_t block_bytes(uint32_t width, uint32_t height)
{
	return ((uint64_t(width) + 7) / 8) * ((uint64_t(height) + 7) / 8) * 16;
}

inline uint32_t read32(const uint8_t * p)
{
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline std::array<uint8_t, header_size> make_header(uint32_t width, uint32_t height, uint32_t payload_bytes, compression encoding)
{
	const uint64_t raw = block_bytes(width, height);
	if (!width || !height || raw > std::numeric_limits<uint32_t>::max() || !payload_bytes || payload_bytes > raw ||
	    (encoding == compression::none && payload_bytes != raw) ||
	    (encoding != compression::none && encoding != compression::lz4 && encoding != compression::zstd))
		throw std::invalid_argument("invalid NX ASTC packet dimensions or size");
	const uint8_t version = encoding == compression::zstd ? 2 : 1;
	std::array<uint8_t, header_size> bytes{'N', 'A', 'S', 'T', version, uint8_t(encoding), 0, 0};
	const std::array values{width, height, uint32_t(raw), payload_bytes};
	for (size_t i = 0; i < values.size(); ++i)
		for (unsigned j = 0; j < 4; ++j)
			bytes[8 + i * 4 + j] = uint8_t(values[i] >> (j * 8));
	return bytes;
}

// Preserve v1 call sites; true has always meant LZ4.
inline std::array<uint8_t, header_size> make_header(uint32_t width, uint32_t height, uint32_t payload_bytes, bool compressed)
{
	return make_header(width, height, payload_bytes, compressed ? compression::lz4 : compression::none);
}

inline std::optional<packet_header> parse_packet(std::span<const uint8_t> bytes)
{
	if (bytes.size() < header_size || bytes[0] != 'N' || bytes[1] != 'A' || bytes[2] != 'S' || bytes[3] != 'T' ||
	    bytes[6] || bytes[7] || !((bytes[4] == 1 && bytes[5] <= 1) || (bytes[4] == 2 && bytes[5] == 2)))
		return {};
	packet_header h{read32(bytes.data() + 8), read32(bytes.data() + 12), read32(bytes.data() + 16), read32(bytes.data() + 20), compression(bytes[5])};
	if (!h.width || !h.height || block_bytes(h.width, h.height) != h.raw_bytes || !h.payload_bytes || h.payload_bytes > h.raw_bytes ||
	    (h.encoding == compression::none && h.payload_bytes != h.raw_bytes) || bytes.size() - header_size != h.payload_bytes)
		return {};
	return h;
}
} // namespace wivrn::nxastc_packet
