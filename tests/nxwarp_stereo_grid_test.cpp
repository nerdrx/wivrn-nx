// The transport's tile grid over an eye PAIR, and the transposition that hid in it.
//
// nxvc reports a stream's geometry with a deliberate asymmetry ([SYN] 3.3 -- "a picture is
// one eye"): `tiles_x` and `tiles_y` are PER EYE, while `tile_count` spans the pair. The
// transport's grid is over the pair. So the derivation is
//
//     cols = eyes * tiles_x
//     rows = tiles_y
//
// and the form that was actually in the tree,
//
//     cols = tiles_x
//     rows = tile_count / tiles_x
//
// is the same numbers with the axes swapped: at eyes == 2 it says 17x34 where the truth is
// 34x17. Both give 578 tiles, so nothing overflows, nothing asserts, and every buffer is
// the right size. What moves is where the BAND boundaries fall and therefore which nonce
// every datagram is sealed with -- and the receiver derives its own from its own copy of
// the same struct. The two ends disagree silently and the symptom is an authentication
// failure on every datagram, which names nothing.
//
// The two forms coincide exactly when eyes == 1, which is every stream this fork has ever
// sent, which is why it survived. This test pins the pair.
//
// Build:
//   g++ -std=c++23 -I common -I server \
//       -isystem /run/media/nerdrx/Lex/claude/nx-scratch/nxwarp-install-e2e/include \
//       -o nxwarp_stereo_grid_test tests/nxwarp_stereo_grid_test.cpp
//   ./nxwarp_stereo_grid_test

#include "nxwarp_stream_grid.h"

#include "nxvc/transport/common.h"

#include <cstdint>
#include <cstdio>
#include <vector>

static int failures = 0;

#define CHECK(cond)                                                          \
	do                                                                   \
	{                                                                    \
		if (not(cond))                                               \
		{                                                            \
			std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                          \
		}                                                            \
	} while (0)

#define CHECK_EQ(a, b)                                                       \
	do                                                                   \
	{                                                                    \
		const long long va = (long long)(a), vb = (long long)(b);    \
		if (va != vb)                                                \
		{                                                            \
			std::printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", \
			            __FILE__, __LINE__, #a, #b, va, vb);     \
			++failures;                                          \
		}                                                            \
	} while (0)

// What nxvc_vkd_stream_info hands back: per-eye tiles_x/tiles_y, pair-wide tile_count.
struct stream_info
{
	uint32_t eyes, tiles_x, tiles_y, tile_count;
};

static stream_info info_for(uint32_t eye_w, uint32_t eye_h, uint32_t eyes)
{
	const uint32_t tx = (eye_w + 63) / 64, ty = (eye_h + 63) / 64;
	return {eyes, tx, ty, eyes * tx * ty};
}

// THE derivation, exactly as client/decoder/nxwarp/nxwarp_decoder.cpp on_stream_header()
// and server/encoder/nxwarp_codec_ref.cpp tile_grid() must both compute it.
static nxt::StreamConfig cfg_for(const stream_info & si, uint8_t stream_id = 0)
{
	// THE shared helper both ends call -- this test exercises the real definition,
	// not a copy of it.
	const auto grid = wivrn::nxwarp_tile_grid(si.tiles_x, si.tiles_y, si.eyes);
	nxt::StreamConfig cfg;
	cfg.stream_id = stream_id;
	cfg.cols = grid.cols;
	cfg.rows = grid.rows;
	cfg.band_rows = wivrn::nxwarp_band_rows(grid.rows);
	cfg.layers = 1;
	cfg.mtu = 1280;
	cfg.caps = nxt::kCapFec | nxt::kCapPoseHdr | nxt::kCapRleFeedback;
	return cfg;
}

int main()
{
	std::printf("nxwarp stereo tile grid\n");

	// ---- A. the headset's real configuration, both ways round.
	{
		const auto mono = info_for(1088, 1088, 1);
		const auto pair = info_for(1088, 1088, 2);

		CHECK_EQ(mono.tiles_x, 17);
		CHECK_EQ(mono.tiles_y, 17);
		CHECK_EQ(mono.tile_count, 289);
		// tiles_x/tiles_y do NOT change when the eyes pair up; only tile_count does.
		CHECK_EQ(pair.tiles_x, 17);
		CHECK_EQ(pair.tiles_y, 17);
		CHECK_EQ(pair.tile_count, 578);

		const auto cm = cfg_for(mono), cp = cfg_for(pair);
		CHECK_EQ(cm.cols, 17);
		CHECK_EQ(cm.rows, 17);
		CHECK_EQ(cm.tiles_per_frame(), 289);

		CHECK_EQ(cp.cols, 34); // the pair, not one eye
		CHECK_EQ(cp.rows, 17); // rows are per eye and do not double
		CHECK_EQ(cp.tiles_per_frame(), 578);

		// The bug, named: the old form is a transposition, not a miscount.
		const uint16_t old_cols = uint16_t(pair.tiles_x);
		const uint16_t old_rows = uint16_t(pair.tile_count / pair.tiles_x);
		CHECK_EQ(old_cols, 17);
		CHECK_EQ(old_rows, 34);
		CHECK_EQ(uint32_t(old_cols) * old_rows, cp.tiles_per_frame()); // same total...
		CHECK(old_cols != cp.cols);                                    // ...different grid
		CHECK(old_rows != cp.rows);
	}

	// ---- B. at eyes == 1 the two forms are identical. This is why it went unseen, and it
	// is also the compatibility claim: fixing it must not move a mono stream by one bit.
	{
		const uint32_t sizes[][2] = {{1088, 1088}, {512, 512}, {1920, 1080}, {64, 64}, {2176, 1088}};
		for (const auto & wh: sizes)
		{
			const auto si = info_for(wh[0], wh[1], 1);
			const auto c = cfg_for(si);
			CHECK_EQ(c.cols, si.tiles_x);
			CHECK_EQ(c.rows, si.tile_count / si.tiles_x); // the old form, unchanged
			CHECK_EQ(c.tiles_per_frame(), si.tile_count);
		}
	}

	// ---- C. the grid must cover exactly the tiles nxvc will report, with no hole and no
	// duplicate, and the linear index must be [SYN] 3.3's
	// `row * cols + eye * cols_per_eye + index`.
	{
		const auto si = info_for(1088, 1088, 2);
		const auto cfg = cfg_for(si);
		const uint32_t cols_per_eye = si.tiles_x;

		std::vector<int> seen(cfg.tiles_per_frame(), 0);
		for (uint16_t row = 0; row < cfg.rows; ++row)
			for (uint16_t col = 0; col < cfg.cols; ++col)
			{
				const uint32_t t = cfg.tile_index(row, col);
				CHECK(t < cfg.tiles_per_frame());
				++seen[t];
				// the eye is positional, derived from the column
				const uint32_t eye = col / cols_per_eye;
				const uint32_t idx = col % cols_per_eye;
				CHECK_EQ(t, row * cfg.cols + eye * cols_per_eye + idx);
				CHECK(eye < 2);
				// and the round trip
				CHECK_EQ(cfg.row_of(t), row);
				CHECK_EQ(cfg.col_of(t), col);
			}
		for (uint32_t t = 0; t < seen.size(); ++t)
			CHECK_EQ(seen[t], 1);
	}

	// ---- D. rows are eye-minor, so BOTH eyes of a row live in the same band. That is what
	// keeps a band a contiguous run of tile indices, which is what the packetizer and the
	// reassembler both assume. Under the transposed grid a band held 34 rows of one eye
	// instead of 17 rows of both, which is the same tiles in a different order.
	{
		const auto cfg = cfg_for(info_for(1088, 1088, 2));
		const uint32_t cols_per_eye = 17;
		CHECK_EQ(cfg.bands(), 3); // ceil(17 / 6)

		uint32_t total = 0;
		for (uint8_t b = 0; b < cfg.bands(); ++b)
		{
			const uint16_t first = cfg.first_row_of_band(b);
			const uint16_t n = cfg.rows_in_band(b);
			CHECK_EQ(cfg.tiles_in_band(b), uint32_t(cfg.cols) * n);
			total += cfg.tiles_in_band(b);

			// every band carries both eyes of every row it covers
			for (uint16_t row = first; row < first + n; ++row)
			{
				bool eye0 = false, eye1 = false;
				for (uint16_t col = 0; col < cfg.cols; ++col)
					(col / cols_per_eye ? eye1 : eye0) = true;
				CHECK(eye0 and eye1);
				CHECK_EQ(cfg.band_of_row(row), b);
			}
		}
		CHECK_EQ(total, cfg.tiles_per_frame());
	}

	// ---- E. an odd per-eye width still tiles: the pair's column count is exactly twice the
	// eye's, because each eye is padded to whole tiles on its own before they are put side
	// by side. (nxvc refuses eyes == 2 unless the per-eye width is a multiple of 64, so the
	// seam always falls on a tile boundary -- this pins that the arithmetic agrees.)
	{
		for (uint32_t w = 64; w <= 2048; w += 64)
		{
			const auto si = info_for(w, 1088, 2);
			const auto c = cfg_for(si);
			CHECK_EQ(c.cols, 2 * ((w + 63) / 64));
			CHECK_EQ(c.cols % 2, 0);
			CHECK_EQ(c.tiles_per_frame(), si.tile_count);
		}
	}

	if (failures == 0)
		std::printf("  all checks passed\n");
	else
		std::printf("  %d check(s) FAILED\n", failures);
	return failures == 0 ? 0 : 1;
}
