#include "common/nxwarp_direct_motion.h"
#include "common/nxwarp_direct_motion_regions.h"
#include "common/nxwarp_direct_zstd.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <array>
#include <vector>

using namespace wivrn::nxwarp_direct;

static layout row_layout(bool motion = false)
{
	return {256, 256, 2, true, 256, false, true, true, false, motion, false, true};
}

static std::vector<uint8_t> make_frame(layout l, bool overlap = false, bool native = true, uint8_t bias = 0)
{
	const uint32_t tiles = l.tile_count();
	const uint32_t block_words = native ? (overlap ? native_rgb_words : 2 * native_rgb_words) : 0;
	auto raw = frame_header(tiles, block_words, native_version);
	const uint32_t second = tiles / 2;
	for (uint32_t i = 0; i < tiles; ++i)
	{
		if (native && (i == 0 || i == second))
			append32(raw, 0x20000000u | ((i == second && !overlap) ? native_rgb_words : 0u));
		else
			append32(raw, 0xc0000000u | (i & 0xffffffu));
	}
	for (uint32_t tile = 0; tile < (native ? (overlap ? 1u : 2u) : 0u); ++tile)
	{
		for (uint32_t y = 0; y < 32; ++y)
			for (uint32_t x = 0; x < 128; ++x)
				raw.push_back(uint8_t(bias + ((x + y + tile) & 0xffu)));
		for (unsigned i = 0; i < 4; ++i) raw.push_back(uint8_t(0xa0 + i));
	}
	return raw;
}

static std::vector<uint8_t> make_mixed_frame(layout l, bool alias = false)
{
	constexpr uint32_t normal_offset = 1030, high_native_offset = 1115, words = 2145;
	std::vector<uint8_t> raw = frame_header(l.tile_count(), words, native_version);
	for (uint32_t i = 0; i < l.tile_count(); ++i)
	{
		if (i == 0) append32(raw, 0x20000000u | (alias ? 0u : high_native_offset));
		else if (i == 1) append32(raw, normal_offset); // 80-word peripheral block
		else if (i == l.tile_count() / 2) append32(raw, 0x20000000u);
		else append32(raw, 0xc0123456u);
	}
	for (uint32_t i = 0; i < words * 4; ++i) raw.push_back(uint8_t((i * 29u + 7u) & 0xffu));
	return raw;
}

static std::array<uint8_t, 4> scene_pixel(int x, int y, unsigned eye)
{
	uint32_t h = uint32_t(x) * 0x9e3779b1u ^ uint32_t(y) * 0x85ebca6bu ^ eye * 0xc2b2ae35u;
	h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
	return {uint8_t(h), uint8_t(h >> 8), uint8_t(h >> 16), 255};
}

static std::vector<uint8_t> make_regional_scene(layout l, const motion_region_vectors & shifts)
{
	const uint32_t tiles = l.tile_count(), words = tiles * native_rgb_words;
	auto raw = frame_header(tiles, words, native_version);
	for (uint32_t tile = 0; tile < tiles; ++tile)
		append32(raw, 0x20000000u | (tile * native_rgb_words));
	for (uint32_t tile = 0; tile < tiles; ++tile)
	{
		const unsigned cols = l.width / 32;
		const unsigned eye = tile % (cols * l.eyes) / cols;
		const unsigned tx = tile % cols;
		const unsigned ty = tile / (cols * l.eyes);
		const auto & shift = shifts[motion_region_index(l, int(tx * 32 + 16), int(ty * 32 + 16))];
		for (int y = 0; y < 32; ++y)
			for (int x = 0; x < 32; ++x)
			{
				const auto p = scene_pixel(int(tx * 32) + x + shift.dx, int(ty * 32) + y + shift.dy, eye);
				raw.insert(raw.end(), p.begin(), p.end());
			}
		for (unsigned i = 0; i < 4; ++i) raw.push_back(uint8_t(0xb0 + tile + i));
	}
	return raw;
}

static std::vector<uint8_t> envelope(uint32_t version, std::vector<uint8_t> decoded)
{
	if (version == 2)
	{
		for (size_t i = decoded.size(); i-- > 4;)
			decoded[i] = uint8_t(decoded[i] - decoded[i - 4]);
	}
	else if (version == 3)
	{
		// Malformed fixtures use no tile transform; decoder must reject prefix first.
		for (size_t i = decoded.size(); i-- > 4;)
			decoded[i] = uint8_t(decoded[i] - decoded[i - 4]);
	}
	const size_t bound = ZSTD_compressBound(decoded.size());
	std::vector<uint8_t> wire(16 + bound);
	const size_t packed = ZSTD_compress(wire.data() + 16, bound, decoded.data(), decoded.size(), 3);
	assert(!ZSTD_isError(packed));
	for (unsigned i = 0; i < 4; ++i)
	{
		wire[i] = uint8_t(zstd_magic >> (8 * i));
		wire[4 + i] = uint8_t(version >> (8 * i));
		wire[8 + i] = uint8_t(uint32_t(decoded.size()) >> (8 * i));
		wire[12 + i] = uint8_t(uint32_t(packed) >> (8 * i));
	}
	wire.resize(16 + packed);
	return wire;
}

int main()
{
	auto l = row_layout();
	const auto raw = make_frame(l);
	std::vector<uint8_t> packed, scratch, decoded;
	const auto wire = compress_zstd_row_predicted(l, raw, packed, scratch);
	assert(is_zstd(wire) && read32(wire, 4) == 3);
	const std::vector<uint8_t> wire_copy(wire.begin(), wire.end());
	const bool roundtrip = decompress_zstd(l, wire, decoded);
	if (!roundtrip || decoded != raw)
	{
		size_t i = 0; while (i < decoded.size() && i < raw.size() && decoded[i] == raw[i]) ++i;
		std::fprintf(stderr, "v3 roundtrip=%d wire=%zu raw=%zu decoded=%zu mismatch=%zu got=%u want=%u\\n",
		             roundtrip, wire.size(), raw.size(), decoded.size(), i, i < decoded.size() ? decoded[i] : 0,
		             i < raw.size() ? raw[i] : 0);
	}
	assert(roundtrip && decoded == raw);
	std::vector<uint8_t> wrong(wire_copy.begin(), wire_copy.end());
	wrong.pop_back();
	assert(!decompress_zstd(l, wrong, decoded));
	wrong.assign(wire_copy.begin(), wire_copy.end());
	wrong.push_back(0);
	assert(!decompress_zstd(l, wrong, decoded));
	wrong.assign(wire_copy.begin(), wire_copy.end());
	wrong[8]++;
	assert(!decompress_zstd(l, wrong, decoded));

	// New stream capability still accepts old independent envelopes.
	std::vector<uint8_t> v1, v2, legacy_scratch;
	const auto one = compress_zstd(raw, v1);
	const auto two = compress_zstd_predicted(raw, v2, legacy_scratch);
	assert(decompress_zstd(l, one, decoded) && decoded == raw);
	assert(decompress_zstd(l, two, decoded) && decoded == raw);

	auto no_row = l;
	no_row.native_row_predictor = false;
	assert(!decompress_zstd(no_row, wire_copy, decoded));
	for (auto invalid: {layout{256,256,2,true,256,true,true,true,false,false,false,true},
	                    layout{256,256,2,true,256,false,true,true,true,false,false,true}})
	{
		const auto fallback = compress_zstd_row_predicted(invalid, raw, packed, scratch);
		assert(fallback.size() == raw.size() && !is_zstd(fallback));
	}

	const auto no_native = make_frame(l, false, false);
	const auto fallback = compress_zstd_row_predicted(l, no_native, packed, scratch);
	assert(fallback.size() == no_native.size() && !is_zstd(fallback));
	const auto overlap = make_frame(l, true);
	const auto overlap_fallback = compress_zstd_row_predicted(l, overlap, packed, scratch);
	assert(overlap_fallback.size() == overlap.size() && !is_zstd(overlap_fallback));
	const auto malformed = envelope(3, overlap);
	assert(!decompress_zstd(l, malformed, decoded) && decoded.empty());
	const auto mixed = make_mixed_frame(l);
	std::vector<native_row_range> mixed_ranges;
	assert(native_row_ranges(l, mixed, mixed_ranges) && mixed_ranges.size() == 3);
	assert(mixed_ranges[0].native && !mixed_ranges[1].native && mixed_ranges[2].native);
	const auto mixed_wire = compress_zstd_row_predicted(l, mixed, packed, scratch);
	assert(is_zstd(mixed_wire) && decompress_zstd(l, mixed_wire, decoded) && decoded == mixed);
	const auto mixed_alias = make_mixed_frame(l, true);
	assert(!native_row_ranges(l, mixed_alias, mixed_ranges));
	const auto mixed_alias_fallback = compress_zstd_row_predicted(l, mixed_alias, packed, scratch);
	assert(mixed_alias_fallback.size() == mixed_alias.size() && !is_zstd(mixed_alias_fallback));
	auto packed_layout = l;
	packed_layout.packed_native = true;
	assert(!decompress_zstd(packed_layout, wire_copy, decoded));
	auto checker_layout = l;
	checker_layout.checkerboard = true;
	assert(!decompress_zstd(checker_layout, wire_copy, decoded));

	// Vertical deltas are local to each tile; row zero is raw, tile tail keeps Up4.
	std::vector<native_row_range> ranges;
	assert(native_row_ranges(l, raw, ranges) && ranges.size() == 2);
	auto predictor4 = raw;
	for (size_t i = 4; i < predictor4.size(); ++i) predictor4[i] = uint8_t(raw[i] - raw[i - 4]);
	auto oracle = predictor4;
	for (const auto & range: ranges)
		if (range.native)
		{
			std::copy_n(raw.data() + range.begin, 128, oracle.data() + range.begin);
			for (size_t y = 1; y < 32; ++y)
				for (size_t x = 0; x < 128; ++x)
					oracle[range.begin + y * 128 + x] = uint8_t(raw[range.begin + y * 128 + x] - raw[range.begin + (y - 1) * 128 + x]);
		}
	auto transformed = predictor4;
	native_vertical_forward(raw, transformed, ranges);
	assert(transformed == oracle && transformed != raw);
	native_row_transform(transformed, ranges, true);
	auto post_native_inverse = predictor4;
	for (const auto & range: ranges)
		if (range.native) std::copy_n(raw.data() + range.begin, 4096, post_native_inverse.data() + range.begin);
	assert(transformed == post_native_inverse);

	// Motion residual is still an NXDF byte stream; v3 reverses to exact residual.
	auto ml = row_layout(true);
	const auto previous = make_frame(ml, false, true, 0);
	const auto current = make_frame(ml, false, true, 3);
	motion_native_info previous_info, current_info;
	assert(build_motion_native(ml, previous, previous_info));
	assert(build_motion_native(ml, current, current_info));
	auto residual = motion_residual(ml, previous, previous_info, current, current_info, 0, 0);
	assert(!residual.empty());
	const auto residual_wire = compress_zstd_row_predicted(ml, residual, packed, scratch);
	assert(is_zstd(residual_wire) && read32(residual_wire, 4) == 3);
	assert(decompress_zstd(ml, residual_wire, decoded) && decoded == residual);
	assert(restore_motion(ml, previous, previous_info, decoded, 0, 0) && decoded == current);

	// Four independently moving quadrants survive row compression and NXMV v2.
	auto rl = row_layout(true);
	rl.motion_regions = true;
	const auto region_stream = stream_header(rl, false, true, true);
	const auto parsed_stream = parse_stream(region_stream);
	assert(parsed_stream && parsed_stream->motion && parsed_stream->motion_regions && parsed_stream->native_row_predictor);
	motion_region_vectors zero{};
	motion_region_vectors expected{{{4, 4, 0, 0}, {-4, 4, 0, 0}, {4, -4, 0, 0}, {-4, -4, 0, 0}}};
	const auto region_old = make_regional_scene(rl, zero);
	const auto region_now = make_regional_scene(rl, expected);
	motion_native_info region_old_info, region_now_info;
	assert(build_motion_native(rl, region_old, region_old_info));
	assert(build_motion_native(rl, region_now, region_now_info));
	const auto estimated = estimate_motion_regions(rl, region_old, region_old_info, region_now, region_now_info);
	for (size_t i = 0; i < expected.size(); ++i)
	{
		assert(estimated[i].dx == expected[i].dx && estimated[i].dy == expected[i].dy && estimated[i].hits >= 32);
	}
	auto region_residual = motion_regions_residual(rl, region_old, region_old_info, region_now, region_now_info, estimated);
	assert(!region_residual.empty() && region_residual != region_now);
	const auto region_body = compress_zstd_row_predicted(rl, region_residual, packed, scratch);
	assert(is_zstd(region_body) && read32(region_body, 4) == 3);
	const auto region_wire = make_motion_regions_wire(estimated, 7, region_body);
	const auto region_header = parse_motion_regions_wire(region_wire);
	assert(region_header && region_header->reference == 7);
	assert(decompress_zstd(rl, region_header->body, decoded) && decoded == region_residual);
	assert(restore_motion_regions(rl, region_old, region_old_info, decoded, region_header->vectors) && decoded == region_now);

	std::puts("NXDZ v3 native row predictor: PASS");
}
