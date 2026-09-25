// Optional lossless NXDZ envelope around one independent NXDF unit.
#pragma once
#include "nxwarp_direct.h"
#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>
#include <zstd.h>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

namespace wivrn::nxwarp_direct
{
constexpr uint32_t zstd_magic = 0x5a44584e;

inline bool is_zstd(std::span<const uint8_t> b)
{
	return b.size() >= 4 && read32(b, 0) == zstd_magic;
}

inline std::span<const uint8_t> compress_zstd_impl(std::span<const uint8_t> raw, std::vector<uint8_t> & out,
														 std::vector<uint8_t> * scratch, bool predictor, ZSTD_CCtx * context = nullptr)
{
	const auto original = raw;
	out.clear();
	const size_t max_bytes = layout{4096, 4096, 2, true}.max_frame_bytes();
	if (raw.size() < 16 || raw.size() > max_bytes || raw.size() > UINT32_MAX)
		return original;
	if (predictor)
	{
		if (!scratch) return raw;
		scratch->resize(raw.size());
		auto * const dst = scratch->data();
		const auto * const src = raw.data();
		std::memcpy(dst, src, 4);
		for (size_t i = 4; i < raw.size(); ++i)
			dst[i] = uint8_t(src[i] - src[i - 4]);
		raw = *scratch;
	}
	const size_t bound = ZSTD_compressBound(raw.size());
	if (ZSTD_isError(bound) || bound > UINT32_MAX - 16u)
		return original;
	out.resize(16 + bound);
	const size_t packed = context ? ZSTD_compressCCtx(context, out.data() + 16, bound, raw.data(), raw.size(), 3) :
	                                ZSTD_compress(out.data() + 16, bound, raw.data(), raw.size(), 3);
	if (ZSTD_isError(packed) || packed > UINT32_MAX || 16u + packed > raw.size() ||
	    (16u + packed) * 100u > raw.size() * 95u)
	{
		out.clear();
		return original;
	}
	for (unsigned k = 0; k < 4; ++k)
	{
		out[k] = uint8_t(zstd_magic >> (8 * k));
		out[4 + k] = uint8_t((predictor ? 2u : 1u) >> (8 * k));
		out[8 + k] = uint8_t(uint32_t(raw.size()) >> (8 * k));
		out[12 + k] = uint8_t(uint32_t(packed) >> (8 * k));
	}
	out.resize(16 + packed);
	return out;
}

inline std::span<const uint8_t> compress_zstd(std::span<const uint8_t> raw, std::vector<uint8_t> & out, ZSTD_CCtx * context = nullptr)
{
	return compress_zstd_impl(raw, out, nullptr, false, context);
}

inline std::span<const uint8_t> compress_zstd_predicted(std::span<const uint8_t> raw, std::vector<uint8_t> & out,
													 std::vector<uint8_t> & scratch, ZSTD_CCtx * context = nullptr)
{
	return compress_zstd_impl(raw, out, &scratch, true, context);
}

inline bool native_row_predictor_layout(layout l)
{
	return l.valid() && l.native_row_predictor && l.native_center && !l.packed_native &&
	       l.zstd && l.predictor && !l.checkerboard && l.native_side == 256 &&
	       l.eyes == 2 && l.width >= l.native_side && l.height >= l.native_side;
}

// Return all descriptor-backed block intervals and native RGB888 tile starts.
// Reject aliases before either predictor touches payload bytes.
struct native_row_range { size_t begin, end; bool native; };

inline bool native_row_ranges(layout l, std::span<const uint8_t> raw,
		std::vector<native_row_range> & ranges)
{
	if (!native_row_predictor_layout(l)) return false;
	const auto frame = parse_frame(l, raw);
	if (!frame || checker_frame(raw)) return false;
	ranges.clear();
	ranges.reserve(l.tile_count());
	const size_t blocks_start = frame_header_bytes + frame->descriptors.size();
	for (size_t i = 0; i < l.tile_count(); ++i)
	{
		const uint32_t d = read32(frame->descriptors, i * 4);
		if (d & 0x20000000u)
		{
			if ((d >> 30) != 0 || (d & 0x10000000u)) return false;
			const size_t off = size_t(d & 0x0fffffffu), count = native_rgb_words;
			if (off % 5 || off > frame->blocks.size() / 4 || count > frame->blocks.size() / 4 - off)
				return false;
			const size_t begin = blocks_start + off * 4, end = begin + count * 4;
			ranges.push_back({begin, end, true});
			continue;
		}
		const unsigned mode = d >> 30;
		if (mode == 3) continue;
		const size_t off = d & 0x3fffffffu, count = 80u >> (mode * 2);
		if (off % 5 || off > frame->blocks.size() / 4 || count > frame->blocks.size() / 4 - off)
			return false;
		ranges.push_back({blocks_start + off * 4, blocks_start + (off + count) * 4, false});
	}
	if (std::none_of(ranges.begin(), ranges.end(), [](const auto & r) { return r.native; })) return false;
	std::sort(ranges.begin(), ranges.end(), [](const auto & a, const auto & b) { return a.begin < b.begin; });
	for (size_t i = 1; i < ranges.size(); ++i)
		if (ranges[i].begin < ranges[i - 1].end) return false;
	return true;
}

inline void native_row_delta(uint8_t * dst, const uint8_t * src, bool inverse)
{
#ifdef __aarch64__
	for (size_t x = 0; x < 128; x += 16)
	{
		const uint8x16_t a = vld1q_u8(dst + x), b = vld1q_u8(src + x);
		vst1q_u8(dst + x, inverse ? vaddq_u8(a, b) : vsubq_u8(a, b));
	}
#else
	for (size_t x = 0; x < 128; ++x)
		dst[x] = uint8_t(inverse ? dst[x] + src[x] : dst[x] - src[x]);
#endif
}

inline void native_row_transform(std::span<uint8_t> bytes,
		const std::vector<native_row_range> & ranges, bool inverse)
{
	for (const auto & range: ranges)
	{
		if (!range.native) continue;
		if (inverse)
		{
			for (size_t row = 1; row < 32; ++row)
				native_row_delta(bytes.data() + range.begin + row * 128,
				                 bytes.data() + range.begin + (row - 1) * 128, true);
		}
		else
		{
			for (size_t row = 31; row > 0; --row)
				native_row_delta(bytes.data() + range.begin + row * 128,
				                 bytes.data() + range.begin + (row - 1) * 128, false);
		}
	}
}

inline void native_vertical_forward(std::span<const uint8_t> raw, std::span<uint8_t> encoded,
		const std::vector<native_row_range> & ranges)
{
	for (const auto & range: ranges)
	{
		if (!range.native) continue;
		std::memcpy(encoded.data() + range.begin, raw.data() + range.begin, 128);
		for (size_t row = 1; row < 32; ++row)
		{
			std::memcpy(encoded.data() + range.begin + row * 128,
			            raw.data() + range.begin + row * 128, 128);
			native_row_delta(encoded.data() + range.begin + row * 128,
			                 raw.data() + range.begin + (row - 1) * 128, false);
		}
	}
}

inline std::span<const uint8_t> compress_zstd_row_predicted(layout l, std::span<const uint8_t> raw,
		std::vector<uint8_t> & out, std::vector<uint8_t> & scratch, ZSTD_CCtx * context = nullptr)
{
	const auto original = raw;
	out.clear();
	if (!native_row_predictor_layout(l) || raw.size() < 16 || raw.size() > l.max_frame_bytes() || raw.size() > UINT32_MAX ||
	    raw.data() == scratch.data()) return original;
	std::vector<native_row_range> ranges;
	if (!native_row_ranges(l, raw, ranges)) return original;
	scratch.resize(raw.size());
	std::memcpy(scratch.data(), raw.data(), raw.size());
	for (size_t i = 4; i < scratch.size(); ++i)
		scratch[i] = uint8_t(raw[i] - raw[i - 4]);
	native_vertical_forward(raw, scratch, ranges);
	const size_t bound = ZSTD_compressBound(scratch.size());
	if (ZSTD_isError(bound) || bound > UINT32_MAX - 16u) return original;
	out.resize(16 + bound);
	const size_t packed = context ? ZSTD_compressCCtx(context, out.data() + 16, bound, scratch.data(), scratch.size(), 3) :
	                                ZSTD_compress(out.data() + 16, bound, scratch.data(), scratch.size(), 3);
	if (ZSTD_isError(packed) || packed > UINT32_MAX || (16u + packed) * 100u > raw.size() * 95u)
	{
		out.clear();
		return original;
	}
	for (unsigned k = 0; k < 4; ++k)
	{
		out[k] = uint8_t(zstd_magic >> (8 * k));
		out[4 + k] = uint8_t(3u >> (8 * k));
		out[8 + k] = uint8_t(uint32_t(raw.size()) >> (8 * k));
		out[12 + k] = uint8_t(uint32_t(packed) >> (8 * k));
	}
	out.resize(16 + packed);
	return out;
}

inline bool decompress_zstd_row_predicted(layout l, std::vector<uint8_t> & out)
{
	if (!native_row_predictor_layout(l)) { out.clear(); return false; }
	const size_t prefix = frame_header_bytes + size_t(l.tile_count()) * 4;
	if (prefix < 4 || prefix > out.size()) { out.clear(); return false; }
	// Prefix is untouched by the tile predictor, so decode it first to find ranges.
	auto * const bytes = out.data();
	for (size_t i = 4; i < prefix; ++i)
		bytes[i] = uint8_t(bytes[i] + bytes[i - 4]);
	std::vector<native_row_range> ranges;
	if (!native_row_ranges(l, out, ranges)) { out.clear(); return false; }
	native_row_transform(out, ranges, true);
	size_t pos = prefix;
	for (const auto & range: ranges)
		if (range.native)
		{
			for (size_t i = pos; i < range.begin; ++i)
				bytes[i] = uint8_t(bytes[i] + bytes[i - 4]);
			for (size_t i = range.end - 4; i < range.end; ++i)
				bytes[i] = uint8_t(bytes[i] + bytes[i - 4]);
			pos = range.end;
		}
	for (size_t i = pos; i < out.size(); ++i)
		bytes[i] = uint8_t(bytes[i] + bytes[i - 4]);
	return true;
}

inline bool decompress_zstd(layout l, std::span<const uint8_t> b, std::vector<uint8_t> & out)
{
	const uint32_t version = b.size() >= 8 && is_zstd(b) ? read32(b, 4) : 0;
	if (!l.valid() || b.size() < 16 || !is_zstd(b) || (version != 1 && version != 2 && version != 3) ||
	    ((version == 2 || version == 3) && !l.predictor) || (version == 3 && !native_row_predictor_layout(l)))
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
	if (version == 2)
	{
		// Keep vector state outside this loop so ARM can vectorize the four byte lanes.
		auto * const bytes = out.data();
		for (size_t i = 4; i < decoded; ++i)
			bytes[i] = uint8_t(bytes[i] + bytes[i - 4]);
	}
	if (version == 3) return decompress_zstd_row_predicted(l, out);
	return true;
}
} // namespace wivrn::nxwarp_direct
