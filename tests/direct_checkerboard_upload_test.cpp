#include "nxwarp_direct_checkerboard.h"
#include "nxwarp_direct_checkerboard_upload.h"
#include "direct_checkerboard_upload_reference.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

using namespace wivrn::nxwarp_direct;

static uint32_t word(std::span<const uint8_t> b, size_t i) { return read32(b, i * 4); }
static uint32_t rgb565(uint32_t i) { return (i * 13u + 7u) & 65535u; }
static uint32_t rgb888(uint32_t i, uint32_t seed) { return (0x102030u + i * 0x010307u + seed * 0x20201u) & 0xffffffu; }
static uint32_t selector(uint32_t seed, uint32_t block, uint32_t x, uint32_t y)
{
	uint32_t v = seed * 0x9e3779b9u + block * 0x85ebca6bu + x * 0xc2b2ae35u + y * 0x27d4eb2fu;
	v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15;
	return v & 3u;
}

static bool merge_checkerboard_upload_checked(layout l, std::span<const uint8_t> current,
                                                std::span<const uint8_t> history,
                                                std::vector<uint8_t> & out)
{
	std::vector<uint8_t> expected;
	const bool ref = wivrn::nxwarp_direct_initial::merge_checkerboard_upload(l, current, history, expected);
	const bool actual = wivrn::nxwarp_direct::merge_checkerboard_upload(l, current, history, out);
	assert(actual == ref);
	if (actual) assert(out == expected);
	return actual;
}

static std::vector<uint8_t> full_palettes(layout l, const std::vector<uint32_t> & modes, uint32_t seed)
{
	uint32_t words = 0;
	for (uint32_t mode: modes) if (mode != 3) words += (16u >> (2 * mode)) * 5;
	std::vector<uint8_t> frame = frame_header(l.tile_count(), words);
	uint32_t offset = 0;
	for (uint32_t mode: modes) {
		append32(frame, mode == 3 ? 0xc0123400u + seed : (mode << 30) | offset);
		if (mode != 3) offset += (16u >> (2 * mode)) * 5;
	}
	for (uint32_t tile = 0; tile < modes.size(); ++tile) {
		const uint32_t mode = modes[tile];
		if (mode == 3) continue;
		const uint32_t count = 16u >> (2 * mode);
		for (uint32_t block = 0; block < count; ++block) {
			append32(frame, 0x45670000u | (seed << 8) | (tile << 4) | block);
			for (uint32_t q = 0; q < 4; ++q) {
				uint32_t packed = 0;
				for (uint32_t bit = 0; bit < 16; ++bit) {
					const uint32_t i = q * 16 + bit;
					packed |= selector(seed, block, i % 8, i / 8) << (2 * bit);
				}
				append32(frame, packed);
			}
		}
	}
	return frame;
}

static uint32_t upload_word(std::span<const uint8_t> b, uint32_t tiles, uint32_t i)
{
	return read32(b, 16 + size_t(tiles) * 4 + size_t(i) * 4);
}

static void check_palette_mode(uint32_t mode)
{
	layout l{32, 32, 1, false, 256, true, false, false, true};
	const auto full_now = full_palettes(l, {mode}, 1), full_old = full_palettes(l, {mode}, 2);
	std::vector<uint8_t> current_storage, old_checker_storage, merged;
	const auto current = checkerboard_frame(l, full_now, current_storage, 0);
	const auto old_checker = checkerboard_frame(l, full_old, old_checker_storage, 1);
	assert(merge_checkerboard_upload_checked(l, current, old_checker, merged));
	const uint32_t count = 16u >> (2 * mode), upload_stride = 6;
	assert(word(merged, 0) == checker_upload_magic && word(merged, 1) == checker_upload_version);
	assert(word(merged, 2) == 1 && word(merged, 3) == count * upload_stride);
	assert(word(merged, 4) == (mode << 30));
	for (uint32_t block = 0; block < count; ++block) {
		const uint32_t out = 5 + block * upload_stride;
		assert(word(merged, out) == word(full_now, 5 + block * 5));
		assert(word(merged, out + 1) == word(full_old, 5 + block * 5));
		for (uint32_t y = 0; y < 8; ++y)
			for (uint32_t x = 0; x < 8; ++x) {
				const uint32_t seed = ((x + y) & 1u) ? 2u : 1u;
				const uint32_t expected = selector(seed, block, x, y), i = y * 8 + x;
				assert(((word(merged, out + 2 + i / 16) >> (2 * (i % 16))) & 3u) == expected);
			}
	}
	assert(!parse_frame(l, merged)); // NXDU palette layout is not an NXDF network frame.

	// Reverse phase order: the prior even phase becomes endpoint slot zero.
	std::vector<uint8_t> current_phase1_storage, old_phase0_storage;
	const auto current_phase1 = checkerboard_frame(l, full_now, current_phase1_storage, 1);
	const auto old_phase0 = checkerboard_frame(l, full_old, old_phase0_storage, 0);
	assert(merge_checkerboard_upload_checked(l, current_phase1, old_phase0, merged));
	for (uint32_t block = 0; block < count; ++block) {
		const uint32_t out = 5 + block * upload_stride;
		assert(word(merged, out) == word(full_old, 5 + block * 5));
		assert(word(merged, out + 1) == word(full_now, 5 + block * 5));
		for (uint32_t y = 0; y < 8; ++y)
			for (uint32_t x = 0; x < 8; ++x) {
				const uint32_t seed = ((x + y) & 1u) ? 1u : 2u;
				const uint32_t i = y * 8 + x;
				assert(((word(merged, out + 2 + i / 16) >> (2 * (i % 16))) & 3u) == selector(seed, block, x, y));
			}
	}

	// No history duplicates the current phase's horizontal neighbor for the old half.
	assert(merge_checkerboard_upload_checked(l, current, {}, merged));
	for (uint32_t block = 0; block < count; ++block) {
		const uint32_t out = 5 + block * upload_stride;
		assert(word(merged, out) == word(merged, out + 1));
		for (uint32_t y = 0; y < 8; ++y)
			for (uint32_t x = 0; x < 8; ++x) {
				const uint32_t source_x = ((x + y) & 1u) ? (x ^ 1u) : x;
				const uint32_t expected = selector(1, block, source_x, y), i = y * 8 + x;
				assert(((word(merged, out + 2 + i / 16) >> (2 * (i % 16))) & 3u) == expected);
			}
	}

	// A full history contributes the missing phase too.
	assert(merge_checkerboard_upload_checked(l, current, full_old, merged));
	for (uint32_t block = 0; block < count; ++block) {
		const uint32_t out = 5 + block * upload_stride;
		assert(word(merged, out) == word(full_now, 5 + block * 5));
		assert(word(merged, out + 1) == word(full_old, 5 + block * 5));
	}
}

static std::vector<uint8_t> native_frame(layout l, bool packed, uint32_t seed)
{
	const uint32_t n = l.tile_count(), words = packed ? 515u : native_rgb_words;
	std::vector<uint8_t> frame = frame_header(n, words, native_version);
	append32(frame, 0x20000000u | (packed ? 0x10000000u : 0u)); // first tile: native, optionally packed
	for (uint32_t i = 1; i < n; ++i) append32(frame, 0xc0765432u);
	if (!packed) {
		for (uint32_t i = 0; i < 1024; ++i) append32(frame, rgb888(i, seed));
		append32(frame, 0);
	} else {
		for (uint32_t i = 0; i < 512; ++i)
			append32(frame, rgb565(2 * i + seed) | (rgb565(2 * i + 1 + seed) << 16));
		append32(frame, 0); append32(frame, 0); append32(frame, 0);
	}
	return frame;
}

static uint32_t native_expected(uint32_t i, bool packed, uint32_t seed)
{
	return packed ? rgb565(i + seed) : rgb888(i, seed);
}

static void verify_native_pixels(const std::vector<uint8_t> & merged, layout l, bool packed,
                                 uint32_t phase, bool fallback)
{
	for (uint32_t y = 0; y < 32; ++y)
		for (uint32_t x = 0; x < 32; ++x) {
			uint32_t source_x = x;
			uint32_t seed = ((x + y) & 1u) == phase ? 1u : 2u;
			if (fallback && seed == 2u) { source_x ^= 1u; seed = 1u; }
			const uint32_t source = y * 32 + source_x, at = y * 32 + x;
			const uint32_t expected = native_expected(source, packed, seed);
			const uint32_t got = packed ? (upload_word(merged, l.tile_count(), at / 2) >> (16 * (at & 1u))) & 65535u
			                            : upload_word(merged, l.tile_count(), at);
			assert(got == expected);
		}
}

static void check_native(bool packed)
{
	layout l{256, 256, 2, true, 256, packed, false, false, true};
	const auto full_now = native_frame(l, packed, 1), full_old = native_frame(l, packed, 2);
	std::vector<uint8_t> current_storage, old_storage, merged;
	const auto current = checkerboard_frame(l, full_now, current_storage, 0);
	const auto old_checker = checkerboard_frame(l, full_old, old_storage, 1);
	assert(merge_checkerboard_upload_checked(l, current, old_checker, merged));
	const uint32_t out_words = packed ? 512u : 1024u;
	assert(word(merged, 3) == out_words && word(merged, 4) == (0x20000000u | (packed ? 0x10000000u : 0u)));
	verify_native_pixels(merged, l, packed, 0, false);
	assert(merge_checkerboard_upload_checked(l, current, full_old, merged));
	verify_native_pixels(merged, l, packed, 0, false);
	assert(merge_checkerboard_upload_checked(l, current, {}, merged));
	verify_native_pixels(merged, l, packed, 0, true);

	// Repeat with phase 1 current, phase 0 compact history, full history, and
	// no history so both native checker lattices and fallback parity are covered.
	std::vector<uint8_t> phase1_storage, phase0_storage;
	const auto current_phase1 = checkerboard_frame(l, full_now, phase1_storage, 1);
	const auto old_phase0 = checkerboard_frame(l, full_old, phase0_storage, 0);
	assert(merge_checkerboard_upload_checked(l, current_phase1, old_phase0, merged));
	verify_native_pixels(merged, l, packed, 1, false);
	assert(merge_checkerboard_upload_checked(l, current_phase1, full_old, merged));
	verify_native_pixels(merged, l, packed, 1, false);
	assert(merge_checkerboard_upload_checked(l, current_phase1, {}, merged));
	verify_native_pixels(merged, l, packed, 1, true);

	// A valid but different old tile encoding (palette instead of native) must
	// use the same nearest-current fallback as absent history.
	std::vector<uint8_t> mismatched = frame_header(l.tile_count(), 80, native_version);
	append32(mismatched, 0); // old tile 0 is mode 0, without native flag
	for (uint32_t i = 1; i < l.tile_count(); ++i) append32(mismatched, 0xc0765432u);
	for (uint32_t i = 0; i < 16 * 5; ++i) append32(mismatched, i * 0x10203u);
	assert(parse_frame(l, mismatched));
	std::vector<uint8_t> no_history;
	assert(merge_checkerboard_upload_checked(l, current, {}, no_history));
	assert(merge_checkerboard_upload_checked(l, current, mismatched, merged));
	assert(merged == no_history);
}

int main()
{
	std::vector<uint8_t> unaligned_store(6, 0xa5);
	checker_store32(unaligned_store, 1, 0x12345678u);
	assert((unaligned_store == std::vector<uint8_t>{0xa5, 0x78, 0x56, 0x34, 0x12, 0xa5}));

	for (uint32_t mode = 0; mode < 3; ++mode) check_palette_mode(mode);
	check_native(false);
	check_native(true);

	// Per-tile history mismatch falls back only on the mismatched tile.
	layout two{64, 32, 1, false, 256, true, false, false, true};
	const auto now_full = full_palettes(two, {0, 1}, 3), old_full = full_palettes(two, {0, 0}, 4);
	std::vector<uint8_t> now_storage, merged;
	const auto current = checkerboard_frame(two, now_full, now_storage, 0);
	assert(merge_checkerboard_upload_checked(two, current, old_full, merged));
	assert(word(merged, 4) == 0); // tile 0 mode0 at offset zero
	assert(word(merged, 5) == ((1u << 30) | 96u)); // tile 1 mode1 follows tile 0's 96 words
	// Tile0 matches history and receives distinct phase palettes.
	assert(word(merged, 7) != word(merged, 8));
	// Tile1 mode differs, so both phase endpoints fall back to current.
	assert(word(merged, 6 + 96) == word(merged, 6 + 96 + 1));

	// Solid descriptors survive unchanged and consume no block words.
	layout solid{32, 32, 1, false, 256, true, false, false, true};
	std::vector<uint8_t> solid_frame = frame_header(1, 0, version | checker_flag);
	append32(solid_frame, 0xc0abcdefu);
	assert(merge_checkerboard_upload_checked(solid, solid_frame, {}, merged));
	assert(word(merged, 3) == 0 && word(merged, 4) == 0xc0abcdefu);

	// Invalid current/history flags, truncation, and same-phase history reject.
	const auto valid_full = full_palettes(solid, {2}, 1);
	std::vector<uint8_t> checker_storage;
	const auto valid_checker = checkerboard_frame(solid, valid_full, checker_storage, 0);
	assert(!merge_checkerboard_upload_checked(layout{32, 32, 1}, valid_checker, {}, merged));
	assert(!merge_checkerboard_upload_checked(solid, valid_full, {}, merged));
	assert(!merge_checkerboard_upload_checked(solid, std::span(valid_checker).first(valid_checker.size() - 1), {}, merged));
	std::vector<uint8_t> same_phase_storage;
	const auto same_phase = checkerboard_frame(solid, valid_full, same_phase_storage, 0);
	assert(!merge_checkerboard_upload_checked(solid, valid_checker, same_phase, merged));
	std::vector<uint8_t> wrong_flag(valid_checker.begin(), valid_checker.end());
	wrong_flag[5] |= 4;
	assert(!merge_checkerboard_upload_checked(solid, wrong_flag, {}, merged));

	// A descriptor storm of native aliases must be rejected before allocating an expanded upload.
	layout aliases{4096, 4096, 2, true, 256, false, false, false, true};
	const uint32_t tiles = aliases.tile_count();
	std::vector<uint8_t> alias_frame = frame_header(tiles, 512, native_version | checker_flag);
	for (uint32_t i = 0; i < tiles; ++i) append32(alias_frame, 0x20000000u); // all alias the same valid block
	for (uint32_t i = 0; i < 512; ++i) append32(alias_frame, rgb888(i, 1));
	assert(parse_frame(aliases, alias_frame));
	assert(!merge_checkerboard_upload_checked(aliases, alias_frame, {}, merged));
	assert(merged.empty());
	std::puts("direct checkerboard upload: ok");
}
