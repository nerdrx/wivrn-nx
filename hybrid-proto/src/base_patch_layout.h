// base_patch_layout.h -- the atlas layout math and the CPU model of the base
// patch import, shared by the test and (eventually) the client component.
//
// The layout is nxvw_ring_layout() from nx-warp's
// vk/decoder/inter/inter_layout.h, reproduced here rather than included so this
// prototype does not take a build dependency on the decoder's private headers.
// If the two ever disagree, that header is right and this is the bug.
//
// ---------------------------------------------------------------------------
// WHY THIS COPY STAYS, now that there is a published accessor.
//
// nxvc_vk_encoder_atlas_layout() reports the ring-slot shape, and the SERVER
// path uses it: server/encoder/nxwarp_codec_vk.cpp asks the encoder and keeps
// no arithmetic of its own. This file is deliberately not switched, for three
// reasons, in order of how much they cost to overturn.
//
//   1. THE ACCESSOR NEEDS AN ENCODER. It is a method on a live
//      nxvc_vk_encoder configured for an ATLAS stream. test_base_patch.c
//      creates no encoder and no stream: it builds an atlas image itself, runs
//      base_patch.comp over it, and compares against the model below. Asking
//      the encoder would mean standing one up -- and then the thing under test
//      would be supplying its own expected answer.
//
//   2. THIS FILE IS NOT ONLY A LAYOUT. nxbp_apply_cpu() is the CPU reference
//      the GPU kernel is checked against, and it packs the 64-byte per-tile
//      table as well. The accessor reports geometry and nothing else, so
//      switching would replace a third of this header and leave the rest --
//      which is the part that actually finds bugs.
//
//   3. THE BUILD IS THE POINT. hybrid-proto/build.sh compiles this with
//      `gcc -I<vulkan headers> -lvulkan`, and the arm64 harnesses beside it
//      with the NDK against nothing but the platform. There is no nxvc in that
//      build, no CMake, and no CI; it is a harness you can run on a bare
//      device. Taking the library as a dependency to learn six integers would
//      cost that outright.
//
// WHAT THIS COSTS, stated plainly: this is now the only remaining copy of the
// ring arithmetic, and nothing in a build checks it against the library. The
// mitigation is that the copy was CONFIRMED against the accessor at the moment
// the server side switched, on both shapes the e2e harness exercises --
// 1088x1088 mono giving slot 1775616 u16, planes at 0/1183744/1479680, strides
// 1088/544/544 and a 17x17 grid, and the 320x240 pair giving slot 230400,
// planes at 0/153600/192000, strides 640/320/320 and 5x4 per eye. Both are
// exactly what nxbp_layout_init() computes. If the library's layout ever moves,
// this file does not find out; the rule at the top of this comment is then the
// one that applies, and the server path is already safe because it asks.
//
//   * planes are concatenated; plane p has PER-EYE width planeW[p]
//   * row stride is (planeW[p] * eyes + 1) & ~1, in u16 elements
//   * eye e's sub-picture begins at column e * planeW[p]
//
// The per-tile table index is EYE-MINOR and is NOT the same convention:
//   n = row * cols + eye * cols_per_eye + col      (ATLAS-DECODER.md)
// Pixels are per-eye sub-pictures; the table is interleaved. Both are taken
// from the caller here so neither is derived from the other by accident.

#ifndef NXWARP_BASE_PATCH_LAYOUT_H
#define NXWARP_BASE_PATCH_LAYOUT_H

#include <stdint.h>
#include <string.h>

#define NXBP_TILE 64
#define NXBP_TABLE_BYTES 64
#define NXBP_TABLE_WORDS 16

// flags byte (SYNTAX 13.12.1): bit0 valid, bit1 static, bit2 base_sourced
#define NXBP_FLAG_VALID 0x01u
#define NXBP_FLAG_STATIC 0x02u
#define NXBP_FLAG_BASE_SOURCED 0x04u

typedef struct {
	int width, height;   // PER-EYE luma extent
	int cw, ch;          // PER-EYE chroma extent
	int eyes, nplanes;
	int off[4], stride[4], planeW[4], planeH[4];
	int rowOff[4];       // first image row of each plane, for the image variants
	int slot_u16;
	int imageW, imageH;  // the image variants' extent
	int cols_per_eye, rows, cols;
} nxbp_layout;

static inline int nxbp_ring_stride(int plane_w, int eyes)
{
	return (plane_w * eyes + 1) & ~1;
}

static inline void nxbp_layout_init(nxbp_layout * L, int width, int height,
                                    int cw, int ch, int eyes, int nplanes)
{
	memset(L, 0, sizeof *L);
	L->width = width;
	L->height = height;
	L->cw = cw;
	L->ch = ch;
	L->eyes = eyes;
	L->nplanes = nplanes;
	int o = 0, r = 0, maxw = 0;
	for (int p = 0; p < 4; p++)
	{
		const int pw = (p == 1 || p == 2) ? cw : width;
		const int ph = (p == 1 || p == 2) ? ch : height;
		L->off[p] = o;
		L->stride[p] = nxbp_ring_stride(pw, eyes);
		L->planeW[p] = pw;
		L->planeH[p] = ph;
		L->rowOff[p] = r;
		if (L->stride[p] > maxw)
			maxw = L->stride[p];
		if (p < nplanes)
		{
			o += L->stride[p] * ph;
			r += ph;
		}
	}
	L->slot_u16 = o;
	L->imageW = maxw;
	L->imageH = r;
	L->cols_per_eye = (width + NXBP_TILE - 1) / NXBP_TILE;
	L->rows = (height + NXBP_TILE - 1) / NXBP_TILE;
	L->cols = eyes * L->cols_per_eye;
}

// The eye-minor linear tile index of SYNTAX 3.3.
static inline int nxbp_tile_index(const nxbp_layout * L, int eye, int col, int row)
{
	return row * L->cols + eye * L->cols_per_eye + col;
}

// One patch: which rect of the base picture, which tile position it fills.
typedef struct {
	int32_t x, y;   // base-picture luma origin of the rect
	int32_t n;      // eye-minor linear tile index
	int32_t eye;    // which eye's sub-picture in the atlas
} nxbp_patch;

// ---------------------------------------------------------------- CPU model
//
// The reference the GPU kernel is compared against. `base` is the decoded base
// picture as three 8-bit planes at the PER-EYE geometry (the base picture for
// one eye); `atlas` is slot_u16 u16 elements.

typedef struct {
	const uint8_t * y;
	const uint8_t * cb;
	const uint8_t * cr;
	int y_stride, c_stride;
} nxbp_base_planes;

static inline void nxbp_apply_cpu(const nxbp_layout * L,
                                  const nxbp_base_planes * b,
                                  const nxbp_patch * patches, int npatch,
                                  uint32_t src_frame,
                                  uint16_t * atlas, uint32_t * table)
{
	for (int i = 0; i < npatch; i++)
	{
		const nxbp_patch * p = &patches[i];
		for (int pl = 0; pl < L->nplanes; pl++)
		{
			const int sub = (pl == 1 || pl == 2) ? 1 : 0;
			const int ext = NXBP_TILE >> sub;
			const uint8_t * src = pl == 0 ? b->y : (pl == 1 ? b->cb : b->cr);
			const int sstride = pl == 0 ? b->y_stride : b->c_stride;
			for (int yy = 0; yy < ext; yy++)
			{
				for (int xx = 0; xx < ext; xx++)
				{
					const int sx = (p->x >> sub) + xx;
					const int sy = (p->y >> sub) + yy;
					if (sx >= L->planeW[pl] || sy >= L->planeH[pl])
						continue;
					const int dx = p->eye * L->planeW[pl] + sx;
					const int dy = sy;
					atlas[L->off[pl] + dy * L->stride[pl] + dx] =
					        (uint16_t)src[(size_t)sy * sstride + sx];
				}
			}
		}
		if (table)
		{
			uint32_t * w = table + (size_t)p->n * NXBP_TABLE_WORDS;
			memset(w, 0, NXBP_TABLE_BYTES);
			w[0] = (uint32_t)(1 << 21);       // C = identity, rows 0-1 Q10.21
			w[4] = (uint32_t)(1 << 21);
			w[8] = (uint32_t)(1 << 29);       // row 2 Q2.29
			w[9] = src_frame;                 // src_frame
			// byte 40 gen:u16 = 0 | byte 42 flags:u8 | byte 43 res_level:u8 = 0
			w[10] = (uint32_t)(NXBP_FLAG_VALID | NXBP_FLAG_BASE_SOURCED) << 16;
		}
	}
}

#endif
