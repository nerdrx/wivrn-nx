// Optional lossless NXDZ envelope around one independent NXDF unit.
#pragma once
#include "nxwarp_direct.h"
#include <algorithm>
#include <cstring>
#include <vector>
#include <zstd.h>

namespace wivrn::nxwarp_direct
{
constexpr uint32_t zstd_magic = 0x5a44584e;

inline bool is_zstd(std::span<const uint8_t> b)
{
	return b.size() >= 4 && read32(b, 0) == zstd_magic;
}

inline std::span<const uint8_t> compress_zstd(std::span<const uint8_t> raw, std::vector<uint8_t> & out)
{
	out.clear();
	const size_t max_bytes = layout{4096, 4096, 2, true}.max_frame_bytes();
	if (raw.size() < 16 || raw.size() > max_bytes || raw.size() > UINT32_MAX)
		return raw;
	const size_t bound = ZSTD_compressBound(raw.size());
	if (ZSTD_isError(bound) || bound > UINT32_MAX - 16u)
		return raw;
	out.resize(16 + bound);
	const size_t packed = ZSTD_compress(out.data() + 16, bound, raw.data(), raw.size(), 3);
	if (ZSTD_isError(packed) || packed > UINT32_MAX || 16u + packed > raw.size() ||
	    (16u + packed) * 100u > raw.size() * 95u)
	{
		out.clear();
		return raw;
	}
	for (unsigned k = 0; k < 4; ++k)
	{
		out[k] = uint8_t(zstd_magic >> (8 * k));
		out[4 + k] = uint8_t(1u >> (8 * k));
		out[8 + k] = uint8_t(uint32_t(raw.size()) >> (8 * k));
		out[12 + k] = uint8_t(uint32_t(packed) >> (8 * k));
	}
	out.resize(16 + packed);
	return out;
}

inline bool decompress_zstd(layout l, std::span<const uint8_t> b, std::vector<uint8_t> & out)
{
	if (!l.valid() || b.size() < 16 || !is_zstd(b) || read32(b, 4) != 1)
		return false;
	const uint32_t raw_size = read32(b, 8), packed_size = read32(b, 12);
	if (raw_size < 16 || raw_size > l.max_frame_bytes() || !packed_size ||
	    b.size() != 16ull + packed_size)
		return false;
	const auto payload = b.subspan(16);
	const size_t frame_size = ZSTD_findFrameCompressedSize(payload.data(), payload.size());
	if (ZSTD_isError(frame_size) || frame_size != payload.size() ||
	    ZSTD_getFrameContentSize(payload.data(), payload.size()) != raw_size)
		return false;
	out.resize(raw_size);
	const size_t decoded = ZSTD_decompress(out.data(), out.size(), payload.data(), payload.size());
	if (ZSTD_isError(decoded) || decoded != raw_size)
	{
		out.clear();
		return false;
	}
	return true;
}
} // namespace wivrn::nxwarp_direct
