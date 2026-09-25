// Assemble two checkerboard NXDF frames into the GPU-only NXDU upload layout.
#pragma once
#include "nxwarp_direct.h"

namespace wivrn::nxwarp_direct_initial
{
using namespace ::wivrn::nxwarp_direct;
constexpr uint32_t checker_upload_magic = 0x5544584e; // "NXDU"
constexpr uint32_t checker_upload_version = 1;

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

	out.reserve(frame_header_bytes + size_t(l.tile_count()) * 4 + size_t(total) * 4);
	append32(out, checker_upload_magic);
	append32(out, checker_upload_version);
	append32(out, l.tile_count());
	append32(out, uint32_t(total));
	out.resize(frame_header_bytes + size_t(l.tile_count()) * 4);
	uint32_t words = 0;
	for (uint32_t tile = 0; tile < l.tile_count(); ++tile) {
		const uint32_t d = read32(now->descriptors, size_t(tile) * 4), mode = d >> 30;
		const bool native = d & 0x20000000u, packed = d & 0x10000000u;
		const uint32_t out_offset = words;
		if (mode == 3) {
			// Inline solid tiles carry their color in the descriptor.
		} else if (native) {
			const uint32_t src_offset = d & 0x0fffffffu;
			const uint32_t count = packed ? 512u : 1024u;
			const uint32_t hd = old ? read32(old->descriptors, size_t(tile) * 4) : 0;
			const bool use_old = old && ((d >> 30) == (hd >> 30)) && !((d ^ hd) & 0x30000000u);
			const uint32_t old_offset = hd & 0x0fffffffu;
			auto pixel = [&](const frame_view & v, uint32_t offset, bool checker, uint32_t source_phase,
			                 uint32_t x, uint32_t y) {
				uint32_t index = y * 32 + x;
				if (checker) {
					const uint32_t first_x = source_phase ^ (y & 1u);
					index = y * 16 + (x - first_x) / 2;
				}
				if (!packed) return read32(v.blocks, 4ull * (offset + index));
				const uint32_t pair = read32(v.blocks, 4ull * (offset + index / 2));
				return (pair >> (16 * (index & 1u))) & 65535u;
			};
			for (uint32_t y = 0; y < 32; ++y)
				for (uint32_t x = 0; x < 32; ++x) {
					const uint32_t wanted_phase = (x + y) & 1u;
					uint32_t value;
					if (wanted_phase == phase) value = pixel(*now, src_offset, true, phase, x, y);
					else if (use_old && (old_full || old_phase == wanted_phase))
						value = pixel(*old, old_offset, old_checker, old_phase, x, y);
					else value = pixel(*now, src_offset, true, phase, x ^ 1u, y);
					if (packed && ((y * 32 + x) & 1u) == 0) append32(out, value);
					else if (!packed) append32(out, value);
					if (packed && ((y * 32 + x) & 1u)) {
						const size_t at = out.size() - 4;
						const uint32_t pair = read32(out, at) | (value << 16);
					for (unsigned b = 0; b < 4; ++b) out[at + b] = uint8_t(pair >> (8 * b));
					}
				}
			words += count;
		} else {
			const uint32_t src_offset = d & 0x3fffffffu;
			const uint32_t blocks_per_row = 4u >> mode, block_count = 16u >> (2 * mode);
			const uint32_t hd = old ? read32(old->descriptors, size_t(tile) * 4) : 0;
			const bool use_old = old && (mode == (hd >> 30)) && !((d ^ hd) & 0x30000000u);
			const uint32_t old_offset = hd & 0x3fffffffu;
			auto block_word = [](const frame_view & v, uint32_t offset, uint32_t block, bool checker,
			                     uint32_t x, uint32_t y) {
				const uint32_t stride = checker ? 3u : 5u;
				const uint32_t at = offset + stride * block;
				if (checker) {
					const uint32_t index = y * 4 + x / 2;
					return (read32(v.blocks, 4ull * (at + 1 + index / 16)) >> (2 * (index % 16))) & 3u;
				}
				const uint32_t index = y * 8 + x;
				return (read32(v.blocks, 4ull * (at + 1 + index / 16)) >> (2 * (index % 16))) & 3u;
			};
			for (uint32_t block = 0; block < block_count; ++block) {
				const uint32_t bx = block % blocks_per_row, by = block / blocks_per_row;
				const uint32_t endpoint_now = read32(now->blocks, 4ull * (src_offset + block * 3));
				uint32_t endpoint_phase[2] = {endpoint_now, endpoint_now};
				if (use_old) {
					const uint32_t endpoint_old = read32(old->blocks, 4ull * (old_offset + block * (old_checker ? 3u : 5u)));
					if (phase == 0) endpoint_phase[1] = endpoint_old;
					else endpoint_phase[0] = endpoint_old;
				}
				append32(out, endpoint_phase[0]);
				append32(out, endpoint_phase[1]);
				uint32_t selectors[4]{};
				for (uint32_t y = 0; y < 8; ++y)
					for (uint32_t x = 0; x < 8; ++x) {
						const uint32_t sample_phase = (x + y) & 1u;
					const frame_view * source = &*now;
					uint32_t source_offset = src_offset, source_x = x;
					bool source_checker = true;
					if (sample_phase != phase && use_old && (old_full || old_phase == sample_phase)) {
						source = &*old;
						source_offset = old_offset;
						source_checker = old_checker;
					} else if (sample_phase != phase) source_x = x ^ 1u;
					const uint32_t src_block = (by * 8 + y) / 8 * blocks_per_row + (bx * 8 + x) / 8;
					const uint32_t selector = block_word(*source, source_offset, src_block, source_checker,
					                                     (bx * 8 + source_x) % 8,
					                                     (by * 8 + y) % 8);
					const uint32_t index = y * 8 + x;
					selectors[index / 16] |= selector << (2 * (index % 16));
				}
				for (uint32_t selector: selectors) append32(out, selector);
			}
			words += 6 * block_count;
		}
		const uint32_t descriptor = mode == 3 ? d : (d & 0xf0000000u) | out_offset;
		for (unsigned b = 0; b < 4; ++b) out[frame_header_bytes + size_t(tile) * 4 + b] = uint8_t(descriptor >> (8 * b));
	}
	return words == total;
}
} // namespace wivrn::nxwarp_direct
