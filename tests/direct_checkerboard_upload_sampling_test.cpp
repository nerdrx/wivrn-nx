// Compare GPU-upload sampling with a CPU equivalent of the prior direct shader.
#include "nxwarp_direct_checkerboard.h"
#include "nxwarp_direct_checkerboard_upload.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

using namespace wivrn::nxwarp_direct;
using Bytes = std::vector<uint8_t>;

namespace
{
Bytes read_file(const char * path)
{
	std::ifstream file(path, std::ios::binary);
	return Bytes(std::istreambuf_iterator<char>(file), {});
}

uint32_t rgb565(uint32_t v)
{
	const uint32_t r = (v >> 11) & 31u, g = (v >> 5) & 63u, b = v & 31u;
	return (((r << 3) | (r >> 2)) << 16) |
	       (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}

uint32_t mix_palette(uint32_t endpoints, uint32_t selector)
{
	const uint32_t a = rgb565(endpoints & 65535u), b = rgb565(endpoints >> 16);
	uint32_t result = 0;
	for (unsigned shift: {16u, 8u, 0u})
		result |= ((((3u - selector) * ((a >> shift) & 255u) +
		             selector * ((b >> shift) & 255u) + 1u) /
		            3u) &
		           255u)
		          << shift;
	return result;
}

uint32_t tile_id(const layout & l, uint32_t x, uint32_t y)
{
	const uint32_t cols = (l.width * l.eyes) / 32;
	return (y / 32) * cols + x / 32;
}

uint32_t descriptor(const layout & l, const frame_view & frame, uint32_t x, uint32_t y)
{
	return read32(frame.descriptors, size_t(tile_id(l, x, y)) * 4);
}

bool has_sample(bool checker, uint32_t phase, uint32_t x, uint32_t y, uint32_t shift)
{
	return !checker || ((((x >> shift) ^ (y >> shift)) & 1u) == phase);
}

uint32_t sample_source(const layout & l, std::span<const uint8_t> frame, const frame_view & parsed, uint32_t x, uint32_t y, bool nearest_current)
{
	const bool checker = checker_frame(frame);
	const uint32_t phase = checker_phase(frame);
	const uint32_t d = descriptor(l, parsed, x, y), mode = d >> 30;
	if (mode == 3u)
		return d & 0xffffffu;
	const bool native = d & 0x20000000u, packed = d & 0x10000000u;
	const uint32_t shift = native ? 0u : mode;
	uint32_t qx = (x % 32) >> shift, qy = (y % 32) >> shift;
	if (nearest_current && checker && !has_sample(true, phase, x, y, shift))
		qx ^= 1u; // Prior direct shader uses q.x ^= 1 for nearest current sample.

	const uint32_t offset = d & (native ? 0x0fffffffu : 0x3fffffffu);
	if (native)
	{
		const uint32_t px = qx, py = qy;
		uint32_t index = py * 32 + px;
		if (checker)
		{
			const uint32_t first_x = phase ^ (py & 1u);
			index = py * 16 + (px - first_x) / 2;
		}
		if (!packed)
			return read32(parsed.blocks, size_t(offset + index) * 4) & 0xffffffu;
		const uint32_t pair = read32(parsed.blocks, size_t(offset + index / 2) * 4);
		return rgb565((pair >> (16 * (index & 1u))) & 65535u);
	}

	const uint32_t blocks_per_row = 4u >> shift;
	const uint32_t stride = checker ? 3u : 5u;
	const uint32_t block = offset + stride * ((qy / 8) * blocks_per_row + qx / 8);
	const uint32_t endpoints = read32(parsed.blocks, size_t(block) * 4);
	uint32_t index = (qy % 8) * 8 + qx % 8;
	if (checker)
	{
		const uint32_t local_x = qx % 8, local_y = qy % 8;
		const uint32_t first_x = phase ^ (local_y & 1u);
		index = local_y * 4 + (local_x - first_x) / 2;
	}
	const uint32_t packed_selectors = read32(parsed.blocks, size_t(block + 1 + index / 16) * 4);
	const uint32_t selector = (packed_selectors >> (2 * (index % 16))) & 3u;
	return mix_palette(endpoints, selector);
}

// CPU equivalent of HEAD's old reprojection_direct.frag.glsl sampler.
uint32_t sample_old_shader(const layout & l, std::span<const uint8_t> current, const frame_view & current_view, std::span<const uint8_t> history, const frame_view * history_view, uint32_t x, uint32_t y)
{
	const uint32_t d = descriptor(l, current_view, x, y), mode = d >> 30;
	const uint32_t shift = (d & 0x20000000u) ? 0u : mode;
	if (mode == 3u || has_sample(checker_frame(current), checker_phase(current), x, y, shift))
		return sample_source(l, current, current_view, x, y, false);

	if (history_view)
	{
		const uint32_t old = descriptor(l, *history_view, x, y);
		const bool same_mode = (d >> 30) == (old >> 30) && !((d ^ old) & 0x30000000u);
		if (same_mode && has_sample(checker_frame(history), checker_phase(history), x, y, shift))
			return sample_source(l, history, *history_view, x, y, false);
	}
	return sample_source(l, current, current_view, x, y, true);
}

uint32_t sample_merged_upload(const layout & l, std::span<const uint8_t> upload, uint32_t x, uint32_t y)
{
	const uint32_t d = read32(upload, frame_header_bytes + size_t(tile_id(l, x, y)) * 4);
	const uint32_t mode = d >> 30;
	if (mode == 3u)
		return d & 0xffffffu;
	const uint32_t offset = d & (mode == 0u && (d & 0x20000000u) ? 0x0fffffffu : 0x3fffffffu);
	const size_t blocks_base = frame_header_bytes + size_t(l.tile_count()) * 4;
	const auto blocks = upload.subspan(blocks_base);
	if (d & 0x20000000u)
	{
		const uint32_t index = (y % 32) * 32 + x % 32;
		if (!(d & 0x10000000u))
			return read32(blocks, size_t(offset + index) * 4) & 0xffffffu;
		const uint32_t pair = read32(blocks, size_t(offset + index / 2) * 4);
		return rgb565((pair >> (16 * (index & 1u))) & 65535u);
	}

	const uint32_t shift = mode;
	const uint32_t qx = (x % 32) >> shift, qy = (y % 32) >> shift;
	const uint32_t blocks_per_row = 4u >> shift;
	const uint32_t block = offset + 6u * ((qy / 8) * blocks_per_row + qx / 8);
	const uint32_t endpoints = read32(blocks, size_t(block + ((qx ^ qy) & 1u)) * 4);
	const uint32_t index = (qy % 8) * 8 + qx % 8;
	const uint32_t packed = read32(blocks, size_t(block + 2 + index / 16) * 4);
	const uint32_t selector = (packed >> (2 * (index % 16))) & 3u;
	return mix_palette(endpoints, selector);
}

struct mismatch_counts
{
	uint64_t pixels = 0;
	uint64_t mismatched = 0;
	uint32_t max_channel_delta = 0;
};

void compare_pixel(mismatch_counts & result, uint32_t actual, uint32_t expected)
{
	++result.pixels;
	const uint32_t dr = std::abs(int((actual >> 16) & 255u) - int((expected >> 16) & 255u));
	const uint32_t dg = std::abs(int((actual >> 8) & 255u) - int((expected >> 8) & 255u));
	const uint32_t db = std::abs(int(actual & 255u) - int(expected & 255u));
	const uint32_t delta = std::max({dr, dg, db});
	result.max_channel_delta = std::max(result.max_channel_delta, delta);
	if (delta)
		++result.mismatched;
}

template <typename Expected>
mismatch_counts compare_upload(const layout & l, std::span<const uint8_t> upload, Expected expected)
{
	mismatch_counts result;
	for (uint32_t y = 0; y < l.height; ++y)
		for (uint32_t x = 0; x < l.width * l.eyes; ++x)
			compare_pixel(result, sample_merged_upload(l, upload, x, y), expected(x, y));
	return result;
}

template <typename Expected>
mismatch_counts compare_old(const layout & l, std::span<const uint8_t> current, const frame_view & current_view, std::span<const uint8_t> history, const frame_view * history_view, Expected expected)
{
	mismatch_counts result;
	for (uint32_t y = 0; y < l.height; ++y)
		for (uint32_t x = 0; x < l.width * l.eyes; ++x)
			compare_pixel(result, sample_old_shader(l, current, current_view, history, history_view, x, y), expected(x, y));
	return result;
}

void print_result(std::string_view scenario, uint32_t phase, const mismatch_counts & merged, const mismatch_counts & old)
{
	std::printf("{\"scenario\":\"%.*s\",\"phase\":%u,\"pixels\":%llu,"
	            "\"merged_mismatch_pixels\":%llu,\"old_mismatch_pixels\":",
	            int(scenario.size()),
	            scenario.data(),
	            phase,
	            static_cast<unsigned long long>(merged.pixels),
	            static_cast<unsigned long long>(merged.mismatched));
	if (old.pixels)
		std::printf("%llu", static_cast<unsigned long long>(old.mismatched));
	else
		std::printf("null");
	std::printf(",\"merged_max_channel_delta\":%u,\"old_max_channel_delta\":",
	            merged.max_channel_delta);
	if (old.pixels)
		std::printf("%u", old.max_channel_delta);
	else
		std::printf("null");
	std::puts("}");
}

bool run_case(const layout & l, std::string_view name, uint32_t phase, std::span<const uint8_t> current_full, std::span<const uint8_t> history_full, bool history_checker, bool same_frame, const frame_view & full_current, const frame_view & full_history)
{
	Bytes current_storage, history_storage, upload;
	const auto current = checkerboard_frame(l, current_full, current_storage, phase);
	std::span<const uint8_t> history;
	if (history_checker)
		history = checkerboard_frame(l, history_full, history_storage, phase ^ 1u);
	else if (!history_full.empty())
		history = history_full;
	if (current.empty() || (!history.empty() &&
	                        !merge_checkerboard_upload(l, current, history, upload)))
		return false;
	if (history.empty() && !merge_checkerboard_upload(l, current, {}, upload))
		return false;
	const auto current_view = parse_frame(l, current);
	const auto history_view = history.empty() ? std::optional<frame_view>{} : parse_frame(l, history);
	if (!current_view || (!history.empty() && !history_view))
		return false;

	const auto merged_vs_old = compare_upload(l, upload, [&](uint32_t x, uint32_t y) {
		return sample_old_shader(l, current, *current_view, history, history_view ? &*history_view : nullptr, x, y);
	});
	print_result(name, phase, merged_vs_old, {});

	if (same_frame)
	{
		const auto merged_vs_full = compare_upload(l, upload, [&](uint32_t x, uint32_t y) {
			return sample_source(l, current_full, full_current, x, y, false);
		});
		const auto old_vs_full = compare_old(l, current, *current_view, history, history_view ? &*history_view : nullptr, [&](uint32_t x, uint32_t y) {
			return sample_source(l, current_full, full_current, x, y, false);
		});
		print_result("same-frame-vs-full", phase, merged_vs_full, old_vs_full);
		return merged_vs_full.mismatched == 0 && old_vs_full.mismatched == 0 &&
		       merged_vs_old.mismatched == 0;
	}
	return merged_vs_old.mismatched == 0;
}
} // namespace

int main(int argc, char ** argv)
{
	if (argc == 5 && std::string_view(argv[1]) == "--actual")
	{
		const layout actual_layout{2176, 2176, 2, true, 256, false, false, false, true};
		const Bytes now = read_file(argv[2]), old = read_file(argv[3]), upload = read_file(argv[4]);
		const auto now_view = parse_frame(actual_layout, now), old_view = parse_frame(actual_layout, old);
		if (!now_view || !old_view || !checker_frame(now) || !checker_frame(old) ||
		    checker_phase(now) == checker_phase(old) || upload.size() < frame_header_bytes ||
		    read32(upload, 0) != checker_upload_magic || read32(upload, 8) != actual_layout.tile_count())
		{
			std::fprintf(stderr, "invalid actual checkerboard capture\n");
			return 2;
		}
		Bytes regenerated;
		if (!merge_checkerboard_upload(actual_layout, now, old, regenerated))
			return 2;
		const bool identical = regenerated == upload;
		const auto compared = compare_upload(actual_layout, upload, [&](uint32_t x, uint32_t y) {
			return sample_old_shader(actual_layout, now, *now_view, old, &*old_view, x, y);
		});
		print_result("actual-captured-upload-vs-old-shader", checker_phase(now), compared, {});
		std::printf("{\"scenario\":\"actual-upload-bytes\",\"recorded_bytes\":%zu,"
		            "\"regenerated_bytes\":%zu,\"byte_identical\":%s}\n",
		            upload.size(),
		            regenerated.size(),
		            identical ? "true" : "false");
		return compared.mismatched == 0 && identical ? 0 : 1;
	}
	if (argc != 3)
	{
		std::fprintf(stderr, "usage: %s current-full.nxdf history-full.nxdf\n"
		                     "       %s --actual checker-now.nxdf checker-old.nxdf checker-upload.nxdu\n",
		             argv[0],
		             argv[0]);
		return 2;
	}
	const layout l{2176, 2176, 2, true, 256, false, false, false, true};
	const Bytes current = read_file(argv[1]), history = read_file(argv[2]);
	if (current.empty() || history.empty() || !parse_frame(l, current) || !parse_frame(l, history) ||
	    checker_frame(current) || checker_frame(history))
	{
		std::fprintf(stderr, "invalid full-frame fixture\n");
		return 2;
	}
	const auto current_view = parse_frame(l, current), history_view = parse_frame(l, history);
	if (!current_view || !history_view)
		return 2;
	bool valid = true;
	for (uint32_t phase = 0; phase != 2; ++phase)
	{
		valid &= run_case(l, "same-frame-checker-history", phase, current, current, true, true, *current_view, *current_view);
		valid &= run_case(l, "different-frame-checker-history", phase, current, history, true, false, *current_view, *history_view);
		valid &= run_case(l, "no-history-nearest", phase, current, {}, false, false, *current_view, *history_view);
		valid &= run_case(l, "different-frame-full-history", phase, current, history, false, false, *current_view, *history_view);
	}
	return valid ? 0 : 1;
}
