// Assemble two checkerboard NXDF frames into the GPU-only NXDU upload layout.
#pragma once
#include "nxwarp_direct.h"

#include <bit>
#include <cstring>

namespace wivrn::nxwarp_direct
{
constexpr uint32_t checker_upload_magic = 0x5544584e; // "NXDU"
constexpr uint32_t checker_upload_version = 1;

inline uint32_t checker_spread4(uint32_t v)
{
	v = (v | (v << 4)) & 0x0f0fu;
	return (v | (v << 2)) & 0x3333u;
}

inline uint32_t checker_row(std::span<const uint8_t> blocks, uint32_t offset, uint32_t block,
                            uint32_t row, bool compact)
{
	const uint32_t stride = compact ? 3u : 5u;
	const uint32_t word = offset + block * stride + 1 + (compact ? row / 4 : row / 2);
	const uint32_t shift = compact ? (row % 4) * 8 : (row % 2) * 16;
	return (read32(blocks, size_t(word) * 4) >> shift) & (compact ? 0xffu : 0xffffu);
}

inline void checker_store32(std::vector<uint8_t> & out, size_t at, uint32_t value)
{
	if constexpr (std::endian::native == std::endian::little)
		std::memcpy(out.data() + at, &value, sizeof(value));
	else
		for (unsigned b = 0; b < 4; ++b) out[at + b] = uint8_t(value >> (8 * b));
}

inline bool merge_checkerboard_upload(layout l, std::span<const uint8_t> current,
                                      std::span<const uint8_t> history,
                                      std::vector<uint8_t> & out)
{
	out.clear();
	if (!l.valid() || !l.checkerboard || (l.native_center &&
	    (l.eyes != 2 || l.width < l.native_side || l.height < l.native_side ||
	     (l.native_side != 128 && l.native_side != 256) ||
	     ((l.packed_native || l.zstd) && l.native_side != 256)))) return false;
	const auto now = parse_frame(l, current);
	if (!now || !checker_frame(current)) return false;
	const uint32_t phase = checker_phase(current);
	std::optional<frame_view> old;
	if (!history.empty()) {
		old = parse_frame(l, history);
		if (!old || (checker_frame(history) && checker_phase(history) == phase)) return false;
	}
	const bool old_checker = old && checker_frame(history);
	const uint32_t old_phase = old_checker ? checker_phase(history) : 0;
	const bool old_full = old && !old_checker;
	const uint64_t max_words = uint64_t(l.max_block_words()) * 2;
	uint64_t total = 0;
	for (uint32_t tile = 0; tile < l.tile_count(); ++tile) {
		const uint32_t d = read32(now->descriptors, size_t(tile) * 4), mode = d >> 30;
		total += d & 0x20000000u ? ((d & 0x10000000u) ? 512u : 1024u)
		                        : mode == 3 ? 0u : (96u >> (2 * mode));
		if (total > max_words || total > UINT32_MAX) return false;
	}

	const size_t descriptors_end = frame_header_bytes + size_t(l.tile_count()) * 4;
	out.resize(descriptors_end + size_t(total) * 4);
	checker_store32(out, 0, checker_upload_magic);
	checker_store32(out, 4, checker_upload_version);
	checker_store32(out, 8, l.tile_count());
	checker_store32(out, 12, uint32_t(total));
	uint32_t words = 0;
	auto put = [&](uint32_t value) {
		checker_store32(out, descriptors_end + size_t(words) * 4, value);
		++words;
	};
	for (uint32_t tile = 0; tile < l.tile_count(); ++tile) {
		const uint32_t d = read32(now->descriptors, size_t(tile) * 4), mode = d >> 30;
		const bool native = d & 0x20000000u, packed = d & 0x10000000u;
		const uint32_t out_offset = words;
		if (mode == 3) {
			// Inline solid tiles carry their color in the descriptor.
		} else if (native) {
			const uint32_t src_offset = d & 0x0fffffffu;
			const uint32_t hd = old ? read32(old->descriptors, size_t(tile) * 4) : 0;
			const bool use_old = old && ((d >> 30) == (hd >> 30)) && !((d ^ hd) & 0x30000000u);
			const uint32_t old_offset = hd & 0x0fffffffu;
			auto sample = [&](const frame_view & v, uint32_t offset, uint32_t index) {
				if (!packed) return read32(v.blocks, 4ull * (offset + index));
				const uint32_t pair = read32(v.blocks, 4ull * (offset + index / 2));
				return (pair >> (16 * (index & 1u))) & 65535u;
			};
			for (uint32_t y = 0; y < 32; ++y) {
				const bool current_first = (phase ^ y) & 1u;
				for (uint32_t i = 0; i < 16; ++i) {
					const uint32_t current = sample(*now, src_offset, y * 16 + i);
					uint32_t history = current;
					if (use_old && (old_full || old_phase != phase))
						history = sample(*old, old_offset,
						                 old_checker ? y * 16 + i : y * 32 + 2 * i + !current_first);
					const uint32_t even = current_first ? history : current;
					const uint32_t odd = current_first ? current : history;
					if (packed) put(even | (odd << 16));
					else { put(even); put(odd); }
				}
			}
		} else {
			const uint32_t src_offset = d & 0x3fffffffu;
			const uint32_t block_count = 16u >> (2 * mode);
			const uint32_t hd = old ? read32(old->descriptors, size_t(tile) * 4) : 0;
			const bool use_old = old && (mode == (hd >> 30)) && !((d ^ hd) & 0x30000000u);
			const uint32_t old_offset = hd & 0x3fffffffu;
			for (uint32_t block = 0; block < block_count; ++block) {
				const uint32_t endpoint_now = read32(now->blocks, 4ull * (src_offset + block * 3));
				uint32_t endpoint_phase[2] = {endpoint_now, endpoint_now};
				if (use_old) {
					const uint32_t old_stride = old_checker ? 3u : 5u;
					const uint32_t endpoint_old = read32(old->blocks, 4ull * (old_offset + block * old_stride));
					endpoint_phase[phase ^ 1u] = endpoint_old;
				}
				put(endpoint_phase[0]);
				put(endpoint_phase[1]);
				uint32_t paired_rows[4]{};
				for (uint32_t row = 0; row < 8; ++row) {
					const uint32_t current_first = (phase ^ row) & 1u;
					const uint32_t current_spread = checker_spread4(checker_row(now->blocks, src_offset, block, row, true));
					uint32_t row_selectors = current_spread << (2 * current_first);
					if (use_old) {
						const uint32_t old_first = current_first ^ 1u;
						if (old_full) {
						const uint32_t full_row = checker_row(old->blocks, old_offset, block, row, false);
						row_selectors |= full_row & (old_first ? 0xccccu : 0x3333u);
					} else {
						const uint32_t old_spread = checker_spread4(checker_row(old->blocks, old_offset, block, row, true));
						row_selectors |= old_spread << (2 * old_first);
					}
				} else row_selectors |= current_spread << (2 * (current_first ^ 1u));
				paired_rows[row / 2] |= row_selectors << (16 * (row & 1u));
				}
				for (uint32_t row_pair: paired_rows) put(row_pair);
			}
		}
		const uint32_t descriptor = mode == 3 ? d : (d & 0xf0000000u) | out_offset;
		checker_store32(out, frame_header_bytes + size_t(tile) * 4, descriptor);
	}
	return words == total;
}
} // namespace wivrn::nxwarp_direct
