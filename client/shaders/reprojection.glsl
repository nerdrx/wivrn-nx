/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#version 450

layout(push_constant) uniform pc
{
	ivec4 rgb_rect;
	ivec4 a_rect;
	vec4 scale;
	vec4 bias;
	// x: contrast adaptive sharpening strength, 0 disables it
	// y: comfort vignette amount, 0 disables it
	// z, w: normalized radii where the vignette starts and reaches its full amount
	vec4 post;
	// Motion smoothing
	// x: how far along the motion field to move, in units of the interval the field
	//    spans; 0 disables the warp and is what every frame that has a fresh image
	//    or no field uses
	// y: longest vector in the field, as a fraction of the eye image
	// z: frame smoothing blend weight, the share of the *previous* decoded frame mixed
	//    into this one; 0 disables it and prev_rgb is then never read
	// w: edge bleed -- the LEFT edge of this eye's picture in the colour image, in
	//    normalized coordinates, half a texel in. See `deband.w` for the right edge and
	//    for why only x is carried.
	vec4 motion;
	// Ambient bias lighting, and the edge bleed ring packed into the two lanes it left
	// free.
	//
	// NOTHING may be appended to this block. It is 176 bytes and it is at the largest
	// size this client has ever created a pipeline with on a Pico 4; growing it to 192
	// made vkCreateGraphicsPipelines fail with ErrorUnknown on that device's Adreno 650,
	// which killed the whole session -- the display pipeline is built lazily on the first
	// streamed frame, so the decoder runs, four frames arrive, and THEN the client exits.
	// A new parameter goes in a free lane, or the block moves to a uniform buffer; it
	// does not go on the end.
	//
	// x: strength of the peripheral colour wash at the very edge, 0 disables it
	// y: fraction of the half image, from each edge inward, the wash covers
	// z: edge bleed -- width of the invented ring, as a fraction of the half image; 0
	//    disables the ring outright and the pass is then byte identical to not having it
	// w: edge bleed -- the extension mode and the fade distance in one lane, as
	//    `mode + fade`: the mode is 0 none, 1 clamp, 2 fade and is read back with floor(),
	//    the fade distance is a fraction of the ring in [0, 0.25) and is read back with
	//    fract(). They share a lane because there was one lane and two scalars, and
	//    because a mode is an integer and a distance is bounded well below 1, so the two
	//    cannot collide.
	vec4 glow;
	// Debanding, and the low poly display filter. Two unrelated controls share one
	// vec4 because the push constant block is already large and both are single
	// scalars, the same way `post` carries the sharpen strength and the vignette.
	// x: dither strength, in units of one 8-bit step (1/255) of triangular-PDF noise;
	//    0 disables it and the output is then byte identical to no debanding
	// y: low poly filter strength, the share of the region-filtered colour mixed over
	//    the plain sample; 0 disables the filter
	// z: low poly posterise levels per channel; below 2 disables posterising
	// w: edge bleed -- the RIGHT edge of this eye's picture in the colour image, in
	//    normalized coordinates, half a texel in.
	//
	//    Only the horizontal limits are carried, because only the horizontal ones are
	//    ever interesting: when the NX Warp encoder codes both eyes as one stereo frame
	//    the pair sits SIDE BY SIDE in the decoded image and `rgb_rect` normalizes against
	//    the whole of it, so stretching this eye's edge outward against the image border
	//    would walk into the eye beside it. Vertically a picture always spans the whole
	//    image, so the vertical limits are the plain half-texel inset the rest of this
	//    shader already computes from `rgb_rect`.
	vec4 deband;
	// [atlas prototype] xy: one eye's picture size in samples, zw unused.
	vec4 atlas_size;
	// xy: the whole atlas image size in texels, zw: its reciprocal.
	vec4 atlas_geom;
	// x: luma rescale out of u16, y: chroma rescale, z: chroma midpoint offset.
	vec4 atlas_range;
};

#ifdef VERT_SHADER

layout (location = 0) in vec2 vPosition;
layout (location = 1) in uvec2 vUV;

layout(location = 0) out vec4 outUV;
layout(location = 1) out vec2 outPosition;
// How far into the edge bleed ring this fragment is, per axis: 0 everywhere the picture
// is real, rising to 1 at the outer border. Zero everywhere when the ring is off.
layout(location = 2) out vec2 outBleed;

void main()
{
	vec2 p = vPosition;
	vec2 in_ring = vec2(0.0);

	if (glow.z > 0.0)
	{
		// The ring is made out of the grid rather than out of a second draw. Every
		// interior vertex moves inward by 1 / (1 + margin) while the vertices already
		// on the border stay put, so the OUTERMOST band of grid cells -- and only
		// that band -- is stretched across the margin. No new geometry, no extra
		// pass, and the interior is a uniform rescale of what it always was.
		//
		// The border test is a comparison against 1 with a tolerance, because the
		// grid's boundary vertices are placed at exactly +/-1 by the defoveator and
		// float arithmetic on the way here is not exact.
		vec2 at_border = step(vec2(1.0 - 1e-4), abs(p));
		p = mix(p / (1.0 + glow.z), sign(p), at_border);
		in_ring = at_border;
	}

	gl_Position = vec4(p, 0.0, 1.0);
	// The picture's own coordinate, NOT the shrunk one: the vignette and the ambient
	// glow are placed against the image, and shrinking the image must not move them.
	outPosition = vPosition;
	outBleed = in_ring;
	vec2 uv = vUV;
	outUV.xy = (uv + rgb_rect.xy) / rgb_rect.zw;
	outUV.zw = (uv + a_rect.xy) / a_rect.zw;
}
#endif

#ifdef FRAG_SHADER

layout(constant_id = 0) const int alpha = 1;
layout(constant_id = 1) const bool do_srgb = false;
// Contrast adaptive sharpening kernel. false (the default) uses a 5-tap cross,
// which drops the four diagonal taps of the classic 3x3 kernel: on a compressed,
// streamed image the diagonal contribution to the soft min/max is barely visible,
// and halving the taps matters on weak GPUs (Adreno/Pico) where this pass runs on
// every opaque pixel, every vsync. true restores the full "better diagonals" 3x3.
layout(constant_id = 2) const bool cas_full_kernel = false;
// AMD FSR1 spatial upscaling. false (the default) keeps the plain bilinear base tap and
// the whole EASU/RCAS path below compiles out, so an opaque pixel pays zero extra taps and
// the output is byte identical to no FSR. true replaces the base tap with edge-adaptive
// upsampling followed by an RCAS sharpen, which is a dozen-odd extra taps: only worth it
// when the decoded image is meaningfully smaller than the display (foveation, and more so
// with reduced resolution streaming). Baked like cas_full_kernel, so toggling it is a rare
// pipeline rebuild rather than a per-pixel branch.
layout(constant_id = 3) const bool fsr_enable = false;
// "Low poly" display filter: an edge-preserving region filter over the decoded image.
// Baked the same way as fsr_enable above, so with it false the whole kernel below compiles
// out and an opaque pixel pays exactly the one base tap it always did. Toggling it is a
// pipeline rebuild, which is what a settings change already is here.
layout(constant_id = 7) const bool lowpoly_enable = false;
// Which low poly kernel is compiled in. false (the default) is the 17-fetch kernel over a
// 7x7 neighbourhood; true is the dense 37-fetch 5x5. Measured on the Pico 4 (Adreno 650)
// with a standalone probe running this very shader, per frame pair at 2176x1088 total:
// 17-fetch +2.12 ms, 37-fetch +5.16 ms over a 0.32 ms base. Baked like cas_full_kernel.
layout(constant_id = 8) const bool lowpoly_full_kernel = false;

// --- [atlas prototype] --------------------------------------------------------------
//
// The shape the atlas reference model asks the display pass for: instead of sampling a
// finished picture, every fragment finds the TILE it belongs to, reads that tile's own
// warp from a small table (the tile's source pose reduced to a 2D affine plus a rotation,
// which is what a pose difference comes to over one tile), and samples the tile atlas
// through the hardware sampler. Late-latched: the table is whatever arrived, tile by
// tile, and the pass runs at panel rate whether or not any of it is new.
//
// This is a COST prototype driven by a synthetic table, not the real model. What it
// measures is the only thing that is in doubt: what per-tile indirection plus a warped
// bilinear tap costs per fragment on this GPU, at panel resolution and at stream
// resolution. The pixels it produces are meaningless.
// 0 off; 1 sampler over the decoder's R16_UNORM view, bilinear by the sampler;
// 2 imageLoad of the same u16 storage memory with the bilinear done by hand;
// 3 a UNORM8 RGBA copy, one tap and no conversion -- the lower bound the other two
// are worth measuring against.
layout(constant_id = 4) const int atlas_mode = 0;
// Tiles per eye across and down. 17x17 is the model's figure.
layout(constant_id = 5) const int atlas_tiles = 17;
// Which eye's half of the table this pipeline reads. The pass is one draw per eye, as
// the defoveation pass already is; a single draw for both would index this from the
// fragment's position instead and change nothing about the per-fragment cost.
layout(constant_id = 6) const int atlas_eye = 0;

layout(set = 0, binding = 0) uniform sampler2D rgb[alpha + 1];
// One cell per motion vector, covering the whole eye image, sampled with the
// hardware bilinear filter so the warp varies smoothly across cell boundaries
layout(set = 0, binding = 1) uniform sampler2D motion_field;
// Frame smoothing: the colour image of the decoded frame this one replaces, bound with
// the same sampler and the same geometry as rgb[0]. When smoothing is off, or there is
// no usable previous frame, this is bound to rgb[0] itself and motion.z is zero, so the
// mix below never runs and the output is byte identical to not having the feature.
layout(set = 0, binding = 2) uniform sampler2D prev_rgb;

// [atlas prototype] The atlas, in the decoder's own storage layout: the ring layout,
// both eyes side by side with eye e at column e * pw, planes in the coded YCoCg-R domain
// with the chroma at 9-bit range inside u16. Nothing is converted for the display pass;
// the pass rescales, upsamples the chroma and does the inverse transform itself. Binding
// 3 is an R16_UNORM SAMPLED view of that memory and binding 4 a storage view of the very
// same image, so modes 1 and 2 differ in how they read one allocation and in nothing
// else. Binding 6 is the converted RGBA8 copy the other two are priced against.
layout(set = 0, binding = 3) uniform sampler2D atlas_r16;
layout(set = 0, binding = 4, r16) uniform readonly image2D atlas_store;
layout(set = 0, binding = 6) uniform sampler2D atlas_rgba8;

// The per-tile table, ADR-0029 section 1: 64 bytes per tile position per eye, 289 tiles
// per eye, 37 kB for the pair -- a uniform buffer, as the ADR says. Indexing is ROW-MAJOR
// EYE-MINOR, so the eyes interleave within a row: for linear tile n,
// eye = (n % cols) / cols_per_eye. Bytes 0..35 are the composed homography C mapping this
// frame's centred sample coordinates to the tile's source frame; the late-latch
// composition with H(pose_t <- pose_N) is per tile and is done in float on the host.
//
// Four vec4 per tile: rows 0, 1 and 2 of C in .xyz, then src_frame/gen/flags/res_level.
layout(set = 0, binding = 5) uniform atlas_table_t
{
	vec4 e[4 * 289 * 2];
} tbl;

layout(location = 0) in vec4 inUV;
layout(location = 1) in vec2 inPosition;
layout(location = 2) in vec2 inBleed;

layout(location = 0) out vec4 outColor;

// AMD FidelityFX Contrast Adaptive Sharpening, single pass, no scaling. The kernel
// works on the decoded image as it comes out of the sampler, which is still gamma
// encoded, that is the perceptual space CAS expects. The centre tap `e` is the base
// colour already sampled by the caller and is reused here rather than sampled again.
vec3 contrast_adaptive_sharpen(vec2 uv, vec3 e, float sharpness)
{
	vec2 texel = 1.0 / vec2(rgb_rect.zw);

	//   b
	// d e f
	//   h
	vec3 b = texture(rgb[0], uv + vec2(0, -1) * texel).rgb;
	vec3 d = texture(rgb[0], uv + vec2(-1, 0) * texel).rgb;
	vec3 f = texture(rgb[0], uv + vec2(1, 0) * texel).rgb;
	vec3 h = texture(rgb[0], uv + vec2(0, 1) * texel).rgb;

	// Soft min and max over the cross, which counts once (range [0, 1]).
	vec3 mn = min(min(min(d, e), min(f, b)), h);
	vec3 mx = max(max(max(d, e), max(f, b)), h);

	// The signal limit the amplitude is measured against: the cross alone reaches 1;
	// the full kernel folds in the four diagonal taps once more, exactly the original
	// "better diagonals" weighting, and reaches 2.
	float peak = 1.0;
	if (cas_full_kernel)
	{
		// a   c
		//
		// g   i
		vec3 a = texture(rgb[0], uv + vec2(-1, -1) * texel).rgb;
		vec3 c = texture(rgb[0], uv + vec2(1, -1) * texel).rgb;
		vec3 g = texture(rgb[0], uv + vec2(-1, 1) * texel).rgb;
		vec3 i = texture(rgb[0], uv + vec2(1, 1) * texel).rgb;

		mn += min(min(min(mn, a), min(c, g)), i);
		mx += max(max(max(mx, a), max(c, g)), i);
		peak = 2.0;
	}

	// Smooth distance to the signal limit divided by the smooth maximum
	vec3 amplitude = clamp(min(mn, peak - mx) / max(mx, 1e-4), 0.0, 1.0);
	amplitude = sqrt(amplitude);

	// Filter shape, w is negative:
	// 0 w 0
	// w 1 w
	// 0 w 0
	vec3 w = amplitude * (-1.0 / mix(8.0, 5.0, clamp(sharpness, 0.0, 1.0)));
	return clamp(((b + d + f + h) * w + e) / (1.0 + 4.0 * w), 0.0, 1.0);
}

// AMD FidelityFX Super Resolution 1.0 (FSR1), spatial upscaling, following the public
// EASU + RCAS reference math. When the decoded image is smaller than the defoveated
// output (foveation, and more so with reduced resolution streaming) EASU reconstructs the
// colour at the sample position with an edge-adaptive 12-tap filter instead of a plain
// bilinear tap, and RCAS then sharpens it. Everything here compiles out unless fsr_enable
// is set, so it is free when FSR is off.
//
// The whole neighbourhood is read through the attached (YCbCr) sampler like every other
// effect in this pass: a bilinear sample placed exactly on a texel centre returns that
// texel, which is how EASU reads its 4x4 grid without texelFetch. Work is done in the raw
// sampled (gamma) space, the perceptual space FSR expects and the space CAS and the glow
// already use.

// Reciprocal with a tiny-denominator guard so a flat neighbourhood (all diffs zero) yields
// a finite weight of zero rather than the NaN a bare 1/0 would feed forward.
float fsr_rcp(float x)
{
	return 1.0 / max(x, 1e-6);
}

// One EASU directional-analysis contribution, at one of the four nearest texels, weighted
// by the bilinear share of that texel. lA..lE are the centre luma (lC) and its up/left/
// right/down neighbours (lA/lB/lD/lE), following the reference FsrEasuSetF.
void fsr_easu_set(inout vec2 dir, inout float len, vec2 pp, float w, float lA, float lB, float lC, float lD, float lE)
{
	float dc = lD - lC;
	float cb = lC - lB;
	float lenX = fsr_rcp(max(abs(dc), abs(cb)));
	float dirX = lD - lB;
	dir.x += dirX * w;
	lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
	lenX *= lenX;
	len += lenX * w;

	float ec = lE - lC;
	float ca = lC - lA;
	float lenY = fsr_rcp(max(abs(ec), abs(ca)));
	float dirY = lE - lA;
	dir.y += dirY * w;
	lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
	lenY *= lenY;
	len += lenY * w;
}

// One EASU output tap: an anisotropic windowed-sinc weight for the texel at `off` (in
// texels, relative to the fractional sample position `pp` already subtracted by the
// caller), rotated into the detected edge frame, following the reference FsrEasuTapF.
void fsr_easu_tap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c)
{
	vec2 v = vec2(off.x * dir.x + off.y * dir.y,
	              off.x * -dir.y + off.y * dir.x);
	v *= len2;
	float d2 = min(v.x * v.x + v.y * v.y, clp);
	float wB = (2.0 / 5.0) * d2 - 1.0;
	float wA = lob * d2 - 1.0;
	wB *= wB;
	wA *= wA;
	wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
	float w = wB * wA;
	aC += c * w;
	aW += w;
}

// FSR luminance proxy: the reference weights 0.5*R + G + 0.5*B, cheap and gamma-space.
float fsr_luma(vec3 c)
{
	return dot(c, vec3(0.5, 1.0, 0.5));
}

// EASU: edge-adaptive spatial upsampling of the decoded image at normalized coordinate uv.
vec3 fsr_easu(vec2 uv)
{
	vec2 src_size = vec2(rgb_rect.zw);
	vec2 rcp_size = 1.0 / src_size;

	// Sample position in source texels; texel centres sit at integer coordinates here.
	vec2 pp = uv * src_size - 0.5;
	vec2 fp = floor(pp);
	pp -= fp;
	vec2 base = fp + 0.5; // centre of texel fp, in texels

	// 12-tap neighbourhood, named as in the reference:
	//        b  c
	//     e  f  g  h
	//     i  j  k  l
	//        n  o
	// f/g/j/k are the four texels straddling the sample point.
#define FSR_TAP(dx, dy) texture(rgb[0], (base + vec2(dx, dy)) * rcp_size).rgb
	vec3 b = FSR_TAP(0.0, -1.0), c = FSR_TAP(1.0, -1.0);
	vec3 e = FSR_TAP(-1.0, 0.0), f = FSR_TAP(0.0, 0.0), g = FSR_TAP(1.0, 0.0), h = FSR_TAP(2.0, 0.0);
	vec3 i = FSR_TAP(-1.0, 1.0), j = FSR_TAP(0.0, 1.0), k = FSR_TAP(1.0, 1.0), l = FSR_TAP(2.0, 1.0);
	vec3 n = FSR_TAP(0.0, 2.0), o = FSR_TAP(1.0, 2.0);
#undef FSR_TAP

	float bL = fsr_luma(b), cL = fsr_luma(c);
	float eL = fsr_luma(e), fL = fsr_luma(f), gL = fsr_luma(g), hL = fsr_luma(h);
	float iL = fsr_luma(i), jL = fsr_luma(j), kL = fsr_luma(k), lL = fsr_luma(l);
	float nL = fsr_luma(n), oL = fsr_luma(o);

	// Detect the dominant edge direction and its strength from the four nearest texels.
	vec2 dir = vec2(0.0);
	float len = 0.0;
	fsr_easu_set(dir, len, pp, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL); // around f
	fsr_easu_set(dir, len, pp, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);         // around g
	fsr_easu_set(dir, len, pp, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);         // around j
	fsr_easu_set(dir, len, pp, pp.x * pp.y, gL, jL, kL, lL, oL);                 // around k

	// Normalize the direction and turn the strength into an anisotropic kernel shape.
	float dirR = dir.x * dir.x + dir.y * dir.y;
	bool zro = dirR < (1.0 / 32768.0);
	dirR = inversesqrt(max(dirR, 1e-8));
	dirR = zro ? 1.0 : dirR;
	dir.x = zro ? 1.0 : dir.x;
	dir *= dirR;
	len = len * 0.5;
	len *= len;
	float stretch = (dir.x * dir.x + dir.y * dir.y) * fsr_rcp(max(abs(dir.x), abs(dir.y)));
	vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 + (-0.5) * len);
	float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
	float clp = fsr_rcp(lob);

	// Range of the four nearest texels, used to clamp ringing out of the result.
	vec3 mn4 = min(min(f, g), min(j, k));
	vec3 mx4 = max(max(f, g), max(j, k));

	vec3 aC = vec3(0.0);
	float aW = 0.0;
	fsr_easu_tap(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
	fsr_easu_tap(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
	fsr_easu_tap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
	fsr_easu_tap(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
	fsr_easu_tap(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
	fsr_easu_tap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
	fsr_easu_tap(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
	fsr_easu_tap(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
	fsr_easu_tap(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
	fsr_easu_tap(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
	fsr_easu_tap(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
	fsr_easu_tap(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);

	return min(mx4, max(mn4, aC * fsr_rcp(aW)));
}

// RCAS: FSR's robust contrast adaptive sharpening, run on the EASU result. RCAS is the
// close cousin of the CAS pass above; the reference runs it at output resolution against a
// dedicated buffer, but this pass produces one output pixel at a time, so like every other
// sharpen here it reads its cross neighbours from the decoded image (through the sampler).
// The centre `e` is the already-upscaled EASU colour, reused rather than resampled.
// sharpness in [0, 1] scales the lobe; 0 leaves a pure EASU upscale.
vec3 fsr_rcas(vec2 uv, vec3 e, float sharpness)
{
	vec2 texel = 1.0 / vec2(rgb_rect.zw);

	//   b
	// d e f
	//   h
	vec3 b = texture(rgb[0], uv + vec2(0, -1) * texel).rgb;
	vec3 d = texture(rgb[0], uv + vec2(-1, 0) * texel).rgb;
	vec3 f = texture(rgb[0], uv + vec2(1, 0) * texel).rgb;
	vec3 h = texture(rgb[0], uv + vec2(0, 1) * texel).rgb;

	vec3 mn4 = min(min(b, d), min(f, h));
	vec3 mx4 = max(max(b, d), max(f, h));

	// Per-channel distance to the black/white limit, as in the reference (peak = (1, -4)).
	vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1e-4));
	vec3 hitMax = (vec3(1.0) - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1e-4));
	vec3 lobeRGB = max(-hitMin, hitMax);
	// FSR_RCAS_LIMIT is 0.25 - 1/16; the lobe is negative, and the slider scales it.
	float lobe = max(-(0.25 - 1.0 / 16.0), min(max(max(lobeRGB.r, lobeRGB.g), lobeRGB.b), 0.0)) * clamp(sharpness, 0.0, 1.0);

	float rcpL = 1.0 / (4.0 * lobe + 1.0);
	return clamp((lobe * (b + d + f + h) + e) * rcpL, 0.0, 1.0);
}

float sRGB_to_linear(float x)
{
	if (x <= 0.04045)
		return x / 12.92;
	return pow((x + 0.055) / 1.055, 2.4);
}

vec4 sRGB_to_linear_rgba(vec4 x)
{
	return vec4(
	        sRGB_to_linear(x.r),
	        sRGB_to_linear(x.g),
	        sRGB_to_linear(x.b),
	        x.a);
}

// Ambient bias lighting. The streamed image ends at the headset's projection FOV, into
// black void. Over the outermost margin of the eye image, blend the pixel toward a
// softened, edge-biased sample of the frame so the transition to that void is a colour
// wash that matches the scene rather than a hard cutoff: it widens the perceived FOV,
// eases motion sickness and hides the discontinuity when motion smoothing cannot keep
// up.
//
// Cost is kept low for the every-vsync path on weak GPUs (Adreno/Pico): interior pixels
// take zero extra taps (the branch is coherent, hugging the four image edges), and the
// margin band takes only three. It leans on foveation, which stores the periphery at
// far lower resolution than the centre, so a single clamp-to-edge tap pulled outward
// already reads as a smooth patch of colour; the two extra taps only spread it along
// the edge so a bright object near the boundary does not streak inward.
vec3 ambient_glow(vec3 base, vec2 uv, vec2 position, float strength, float margin)
{
	// How deep this pixel is into the margin along each axis: 0 in the interior,
	// growing to 1 at the very edge.
	vec2 e = smoothstep(vec2(1.0 - margin), vec2(1.0), abs(position));
	float w = max(e.x, e.y);
	if (w <= 0.0)
		return base;

	vec2 texel = 1.0 / vec2(rgb_rect.zw);
	vec2 lo = 0.5 * texel;
	vec2 hi = 1.0 - 0.5 * texel;

	// Pull the sample toward the nearest edge along the dominant axis so the boundary
	// colour bleeds inward, and blur along that edge (the tangential axis) so the wash
	// is a smooth colour field, not a mirror of the edge detail.
	const float r = 6.0;  // how far the sample is pulled toward the edge, in texels
	const float rt = 8.0; // tangential blur radius, in texels
	bool vertical_edge = e.x >= e.y;
	vec2 pull = vertical_edge ? vec2(sign(position.x), 0.0) : vec2(0.0, sign(position.y));
	vec2 tangent = vertical_edge ? vec2(0.0, 1.0) : vec2(1.0, 0.0);

	vec2 c = clamp(uv + pull * texel * r, lo, hi);
	vec3 acc = texture(rgb[0], c).rgb;
	acc += texture(rgb[0], clamp(c + tangent * texel * rt, lo, hi)).rgb;
	acc += texture(rgb[0], clamp(c - tangent * texel * rt, lo, hi)).rgb;
	acc *= 1.0 / 3.0;

	// Ramp the wash in over the margin and cap it with the configured strength, so the
	// inner margin stays mostly the real image and only the extreme edge becomes a
	// soft colour field.
	return mix(base, acc, w * strength);
}

// Edge bleed, the sampling side. At a low application frame rate the headset's compositor
// reprojects a late frame to the newest head pose, and where the frame's field of view
// runs out it has nothing to show: a black band sweeps in at the edge of the view, wider
// the later the frame and the faster the head. It is the single most legible symptom of
// lag there is, because it is the one artefact that is not part of the picture.
//
// The real fix is overscan -- the server renders a wider field of view than the panel
// shows, and the reprojection then moves into decoded pixels. This is what happens when it
// is off: the pass draws the ring anyway and fills it out of the picture's own edge. That
// content is invented, and it is a smear if you go looking for it, but it is a smear of
// the right colour and the alternative is black.
//
// Two things happen here, in order:
//
//   clamp -- the sample is pulled to the outermost texel of the side being extended, over
//     the width of the ring, so the last row and column are stretched outward. `t` is 0 at
//     the picture's border and 1 at the ring's outer edge, so the join is continuous: at
//     t = 0 this returns the coordinate the grid already gave.
//
//   fade -- past the fade distance packed in `glow.w`, the stretched colour is blended
//     toward that edge's
//     own averaged colour. A stretch of a hard edge is a streak; a stretch that decays
//     into the colour it came from reads as the picture continuing off the side of the
//     view, which is the whole illusion.
//
// Both are per axis, so a corner does the right thing on both at once.

// The sample coordinate, clamped outward over the ring. `limits` is the sub-rectangle
// THIS eye's picture occupies, split into its x and y halves -- see `deband.w` in the push
// constant block for why that is not simply the whole image.
vec2 bleed_clamp(vec2 uv, vec2 position, vec2 t, vec2 x_limits, vec2 y_limits)
{
	// The outermost texel centre of this eye's picture, on the side this fragment is
	// being extended past.
	vec2 lo = vec2(x_limits.x, y_limits.x);
	vec2 hi = vec2(x_limits.y, y_limits.y);
	vec2 edge = mix(lo, hi, step(0.0, position));
	// Smooth rather than linear so the derivative at the join is zero and the stretch
	// does not start with a visible crease.
	vec2 w = smoothstep(vec2(0.0), vec2(1.0), clamp(t, 0.0, 1.0));
	return mix(uv, edge, w);
}

// That edge's averaged colour: three taps spread ALONG the edge, so the wash is a colour
// field rather than a mirror of whatever detail happened to sit at one texel. Same shape
// as ambient_glow() above and for the same reason; kept separate because that one washes
// the inside of the picture and this one fills the outside of it.
vec3 bleed_edge_colour(vec2 uv, vec2 position, vec2 t)
{
	vec2 texel = 1.0 / vec2(rgb_rect.zw);
	// This eye's picture, not the whole image: the tangential taps below walk along the
	// edge and would otherwise be free to walk off it into the other eye. Horizontally
	// that comes from the packed limits; vertically a picture always spans the image.
	vec2 lo = vec2(motion.w, 0.5 * texel.y);
	vec2 hi = vec2(deband.w, 1.0 - 0.5 * texel.y);

	// Blur along the edge the fragment is nearest to. In a corner either choice is as
	// good as the other, so the deeper axis wins and the two corners of a side agree
	// with the side between them.
	const float rt = 8.0; // tangential blur radius, in texels
	bool vertical_edge = t.x >= t.y;
	vec2 tangent = vertical_edge ? vec2(0.0, 1.0) : vec2(1.0, 0.0);

	vec2 c = clamp(uv, lo, hi);
	vec3 acc = texture(rgb[0], c).rgb;
	acc += texture(rgb[0], clamp(c + tangent * texel * rt, lo, hi)).rgb;
	acc += texture(rgb[0], clamp(c - tangent * texel * rt, lo, hi)).rgb;
	return acc * (1.0 / 3.0);
}

// Motion smoothing. The field is measured, and is given here, in normalized
// coordinates of the defoveated eye image; what this pass samples is the foveated
// image. The two are related by the vertex grid, which maps positions to texture
// coordinates affinely inside every triangle, so the local ratio between them is
// exactly what the screen space derivatives of the two interpolated values give.
//
// The vector says where the content came from over one interval, so moving forward
// by `steps` intervals means sampling that many vectors back.
vec2 motion_offset(vec2 uv, vec2 position)
{
	// Position of this pixel in the defoveated eye image, 0 to 1
	vec2 p = position * 0.5 + 0.5;
	vec2 v = texture(motion_field, p).rg * motion.y;

	vec2 duv_dp = vec2(dFdx(uv.x) / dFdx(p.x), dFdy(uv.y) / dFdy(p.y));
	return -motion.x * v * duv_dp;
}

// Debanding dither. 8-bit output plus video compression quantizes smooth gradients
// (skyboxes, fog, dark rooms) into visible contours, worst of all on an OLED where the
// steps do not blur into each other. Adding a small, high-frequency dither to the colour
// right before the panel quantizes it turns each contour into noise the eye integrates
// back into a clean ramp.
//
// The noise is Jimenez's interleaved gradient noise: a single dot + two fracts, no
// texture taps, with a spectrum that leans blue (high frequency) rather than the flat
// white of a plain hash, so it reads as fine grain rather than static. It depends only on
// the pixel coordinate, so the pattern is static across frames: this composes with the
// vsync cache (a cache hit re-presents an already-dithered image) and there is no
// temporal shimmer. Returns a uniform value in [0, 1).
float dither_noise(vec2 p)
{
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// "Low poly" display filter.
//
// At a low bitrate a transform codec fails by turning the picture into its own coding
// grid: ringing along edges and 8x8/16x16 blocks of the wrong colour drifting frame to
// frame. That failure is the one artefact the eye is least willing to forgive, because
// nothing in the world looks like it. This filter replaces it with a failure the eye
// already has a name for: flat or gently shaded regions meeting at sharp edges, the look
// of a low polygon model. The information is no less lost -- fine texture goes with it --
// but what is left reads as a stylisation rather than as a broken picture.
//
// Both kernels below are the same sectored variance-minimising (Kuwahara) filter: four
// overlapping quadrants of the neighbourhood, each scored by the variance of its luma,
// and the mean colour of the LOWEST variance quadrant is the output. On a flat region
// every quadrant is flat and the result is the region's mean, so block noise and ringing
// average away. Across an edge the quadrants that straddle it score high and lose to the
// one lying wholly on one side, so the edge is not blurred across -- it is snapped to,
// and a pixel one step over picks the quadrant on the other side. That is what makes the
// regions flat AND the boundaries between them sharp.
//
// The taps are one texel of the DECODED image apart (rgb_rect.zw), not of the output: the
// filter is defined at stream resolution and follows the codec's own scale, so it keeps
// working when the display pass runs larger or smaller than the stream. It is applied to
// the base sample, before anything downstream, and takes the place of CAS/FSR rather than
// stacking with them.
//
// DO NOT rewrite either kernel to read its neighbourhood into a local array first. The
// obvious transcription holds 25 vec3 live, which does not fit Adreno's register file:
// measured on the Pico 4 it spills to scratch and costs 250 ms a frame pair instead of 2,
// a 100x cliff with no warning. Both kernels below keep exactly one tap live at a time
// and re-fetch the samples two quadrants share, because a texture cache hit is orders of
// magnitude cheaper than a spill.

// Default kernel: 7x7 support, seventeen fetches. Every tap sits on a TEXEL CORNER, at
// +-0.5 and +-2.5 texels, so the linear sampler returns the mean of a 2x2 block for free
// and each quadrant covers a 4x4 corner of the neighbourhood in four fetches. The
// variance scored is that of the four block means rather than of sixteen samples, which
// is if anything the better statistic here: it responds to edges and ignores exactly the
// fine noise the filter exists to discard. Wider support than the dense 5x5 below and
// 2.4x cheaper.
//
// This needs the decoded image bound with a LINEAR sampler, which every decoder this
// client has does. With a nearest sampler it degrades to four point samples per quadrant
// rather than breaking.
vec3 low_poly_fast(vec2 uv)
{
	vec2 texel = 1.0 / vec2(rgb_rect.zw);

	vec3 best_mean = vec3(0.0);
	float best_var = 3.4e38;
	for (int q = 0; q < 4; ++q)
	{
		// Which way this quadrant faces.
		vec2 s = vec2(float((q & 1) * 2 - 1), float((q >> 1) * 2 - 1));
		vec3 sum = vec3(0.0);
		float ls = 0.0;
		float lss = 0.0;
		for (int j = 0; j < 2; ++j)
		{
			for (int i = 0; i < 2; ++i)
			{
				vec2 off = s * (vec2(0.5) + 2.0 * vec2(float(i), float(j)));
				vec3 c = texture(rgb[0], uv + off * texel).rgb;
				sum += c;
				// Rec.601 luma, on the gamma encoded values the sampler
				// returns, which is the perceptual space every other filter
				// in this pass works in.
				float l = dot(c, vec3(0.299, 0.587, 0.114));
				ls += l;
				lss += l * l;
			}
		}
		float mean_l = ls * 0.25;
		// Population variance. Never negative in exact arithmetic; max() only
		// guards the float cancellation on a flat quadrant.
		float var = max(lss * 0.25 - mean_l * mean_l, 0.0);
		if (var < best_var)
		{
			best_var = var;
			best_mean = sum * 0.25;
		}
	}
	return best_mean;
}

// Full kernel: the dense 5x5, four overlapping 3x3 quadrants sharing the centre tap,
// thirty-seven fetches. Finer regions and a boundary that follows small detail more
// closely, at 2.4x the cost. Worth it only where the GPU has the headroom.
vec3 low_poly_full(vec2 uv)
{
	vec2 texel = 1.0 / vec2(rgb_rect.zw);

	vec3 best_mean = vec3(0.0);
	float best_var = 3.4e38;
	for (int q = 0; q < 4; ++q)
	{
		ivec2 o = ivec2((q & 1) * 2, (q >> 1) * 2) - ivec2(2, 2);
		vec3 sum = vec3(0.0);
		float ls = 0.0;
		float lss = 0.0;
		for (int j = 0; j < 3; ++j)
		{
			for (int i = 0; i < 3; ++i)
			{
				vec3 c = texture(rgb[0], uv + vec2(o + ivec2(i, j)) * texel).rgb;
				sum += c;
				float l = dot(c, vec3(0.299, 0.587, 0.114));
				ls += l;
				lss += l * l;
			}
		}
		float mean_l = ls * (1.0 / 9.0);
		float var = max(lss * (1.0 / 9.0) - mean_l * mean_l, 0.0);
		if (var < best_var)
		{
			best_var = var;
			best_mean = sum * (1.0 / 9.0);
		}
	}
	return best_mean;
}

vec3 low_poly(vec2 uv, vec3 centre, float strength, float levels)
{
	vec3 region = lowpoly_full_kernel ? low_poly_full(uv)
	                                  : low_poly_fast(uv);

	// Strength blends the region colour over the plain sample, so the slider runs from
	// the untouched image to the fully flattened one rather than switching between them.
	vec3 result = mix(centre, region, strength);

	// Optional posterise, after the region filter and never before it: quantizing first
	// would put a step in the middle of a smooth region and the filter would then
	// average across it. `levels` counts the reproducible values per channel including
	// both endpoints, so the step is 1/(levels - 1).
	if (levels >= 2.0)
	{
		float steps = levels - 1.0;
		result = floor(clamp(result, 0.0, 1.0) * steps + 0.5) / steps;
	}

	return result;
}

void main()
{
	vec2 uv = inUV.xy;
	vec2 uv_a = inUV.zw;

	// How deep into the edge bleed ring this fragment is, per axis. Zero for every
	// fragment of the picture itself, and zero everywhere when the ring is off -- the
	// vertex shader only ever makes it nonzero over the outermost band of grid cells, so
	// the branch below is coherent and the interior pays nothing.
	vec2 bleed_t = inBleed;
	// The mode and the fade distance share glow.w as `mode + fade`; see the push constant
	// block. floor() is the mode, fract() the distance.
	float bleed_mode = floor(glow.w);
	float bleed_fade = fract(glow.w);
	bool in_bleed = glow.z > 0.0 && bleed_mode > 0.0 && max(bleed_t.x, bleed_t.y) > 0.0;
	if (in_bleed)
	{
		// Stretch the edge outward. Done before the motion warp so a warped sample and
		// a bled one compose the way they read: the warp moves the picture, the stretch
		// then extends whatever the picture's edge became.
		vec2 texel_rgb = 0.5 / vec2(rgb_rect.zw);
		uv = bleed_clamp(uv, inPosition, bleed_t,
		                 vec2(motion.w, deband.w),
		                 vec2(texel_rgb.y, 1.0 - texel_rgb.y));
		if (alpha == 1)
		{
			// The alpha plane always carries the pair side by side, so this eye's
			// half is derived here the way the motion warp below already derives it,
			// from which side of the split the untouched coordinate sits on.
			vec2 texel_a = 0.5 / vec2(a_rect.zw);
			vec2 x_a = inUV.z > 0.5
			                   ? vec2(0.5 + texel_a.x, 1.0 - texel_a.x)
			                   : vec2(texel_a.x, 0.5 - texel_a.x);
			uv_a = bleed_clamp(uv_a, inPosition, bleed_t, x_a, vec2(texel_a.y, 1.0 - texel_a.y));
		}
	}

	if (motion.x > 0.0)
	{
		vec2 texel = 0.5 / vec2(rgb_rect.zw);
		uv = clamp(uv + motion_offset(inUV.xy, inPosition), texel, 1.0 - texel);

		if (alpha == 1)
		{
			// Disocclusions are filled by stretching the edge of the image
			// rather than by anything cleverer: at the rates this runs at the
			// gap is a few pixels wide and only lasts one application frame.
			vec2 texel_a = 0.5 / vec2(a_rect.zw);
			uv_a = clamp(uv_a + motion_offset(inUV.zw, inPosition), texel_a, 1.0 - texel_a);

			// Keep the sample on this eye's half of the alpha image
			if (inUV.z > 0.5)
				uv_a.x = max(uv_a.x, 0.5 + texel_a.x);
			else
				uv_a.x = min(uv_a.x, 0.5 - texel_a.x);
		}
	}

	// One base tap. When CAS is active it doubles as the kernel's centre `e`, so an
	// opaque pixel pays a single centre sample whether or not it is sharpened. When FSR is
	// active EASU replaces the colour but the tap still supplies colour.a (only read on the
	// no-alpha-stream path below).
	if (atlas_mode != 0)
	{
		// Which tile this fragment lands in. Row-major eye-minor over the pair, so a
		// row of the table alternates eyes; every fragment of a tile reads the same
		// three uniform vectors, which is what the constant cache is for.
		vec2 t = clamp(inUV.xy, 0.0, 0.999999) * float(atlas_tiles);
		ivec2 c = ivec2(t);
		int cols_per_eye = atlas_tiles;
		int n = (c.y * (cols_per_eye * 2)) + atlas_eye * cols_per_eye + c.x;
		int idx = n * 4;
		vec3 r0 = tbl.e[idx].xyz;
		vec3 r1 = tbl.e[idx + 1].xyz;
		vec3 r2 = tbl.e[idx + 2].xyz;

		// This frame's centred sample coordinates, through C, back to the tile's
		// source frame. One homography and one divide: ADR-0029 section 5's "one
		// step", which is the whole point of the model.
		vec2 centred = (inUV.xy - 0.5) * atlas_size.xy;
		vec3 h = vec3(centred, 1.0);
		vec3 q = vec3(dot(r0, h), dot(r1, h), dot(r2, h));
		// Sample position inside THIS eye's picture, in samples.
		vec2 src = clamp(q.xy / q.z + atlas_size.xy * 0.5, vec2(0.0), atlas_size.xy - 1.0);

		vec3 rgb_out;
		if (atlas_mode == 3)
		{
			// Lower bound: a converted RGBA8 copy. One tap, no rescale, no inverse
			// transform, no chroma upsample. Whatever this costs is what the pass
			// can never go below at this output size.
			vec2 uvc = (src + vec2(float(atlas_eye) * atlas_size.x, 0.0)) * atlas_geom.zw;
			rgb_out = texture(atlas_rgba8, uvc).rgb;
		}
		else
		{
			// The luma plane spans the pair: eye e starts at column e * pw. The two
			// chroma planes sit under it at half resolution, Co on the left half of
			// the band and Cg on the right.
			float Y, Co, Cg;
			vec2 luma_px = src + vec2(float(atlas_eye) * atlas_size.x, 0.0);
			vec2 chroma_px = src * 0.5 + vec2(float(atlas_eye) * atlas_size.x * 0.5, atlas_size.y);
			float cg_dx = atlas_size.x;

			if (atlas_mode == 1)
			{
				// Sampler over the R16_UNORM view of the decoder's own memory.
				// Three taps, bilinear by the hardware.
				Y = texture(atlas_r16, (luma_px + 0.5) * atlas_geom.zw).r;
				Co = texture(atlas_r16, (chroma_px + 0.5) * atlas_geom.zw).r;
				Cg = texture(atlas_r16, (chroma_px + vec2(cg_dx, 0.0) + 0.5) * atlas_geom.zw).r;
			}
			else
			{
				// imageLoad of the u16 storage image, bilinear by hand: four loads
				// a plane, twelve for the pixel, plus the weights.
				vec2 lf = fract(luma_px);
				ivec2 li = ivec2(floor(luma_px));
				float y00 = imageLoad(atlas_store, li).r;
				float y10 = imageLoad(atlas_store, li + ivec2(1, 0)).r;
				float y01 = imageLoad(atlas_store, li + ivec2(0, 1)).r;
				float y11 = imageLoad(atlas_store, li + ivec2(1, 1)).r;
				Y = mix(mix(y00, y10, lf.x), mix(y01, y11, lf.x), lf.y);

				vec2 cf = fract(chroma_px);
				ivec2 ci = ivec2(floor(chroma_px));
				float o00 = imageLoad(atlas_store, ci).r;
				float o10 = imageLoad(atlas_store, ci + ivec2(1, 0)).r;
				float o01 = imageLoad(atlas_store, ci + ivec2(0, 1)).r;
				float o11 = imageLoad(atlas_store, ci + ivec2(1, 1)).r;
				Co = mix(mix(o00, o10, cf.x), mix(o01, o11, cf.x), cf.y);

				ivec2 gi = ci + ivec2(int(cg_dx), 0);
				float g00 = imageLoad(atlas_store, gi).r;
				float g10 = imageLoad(atlas_store, gi + ivec2(1, 0)).r;
				float g01 = imageLoad(atlas_store, gi + ivec2(0, 1)).r;
				float g11 = imageLoad(atlas_store, gi + ivec2(1, 1)).r;
				Cg = mix(mix(g00, g10, cf.x), mix(g01, g11, cf.x), cf.y);
			}

			// Out of the raw u16 range and into the codec's: luma is 8-bit stored in
			// 16, chroma is 9-bit signed around the midpoint (SYNTAX YCoCg-R).
			Y = Y * atlas_range.x;
			Co = Co * atlas_range.y - atlas_range.z;
			Cg = Cg * atlas_range.y - atlas_range.z;

			// YCoCg-R inverse.
			float tmp = Y - Cg * 0.5;
			float G = Cg + tmp;
			float B = tmp - Co * 0.5;
			float R = B + Co;
			rgb_out = vec3(R, G, B);
		}

		outColor = vec4(rgb_out, 1.0);
		if (do_srgb)
			outColor = sRGB_to_linear_rgba(outColor);
		return;
	}

	vec4 colour = texture(rgb[0], uv);
	if (lowpoly_enable && deband.y > 0.0)
	{
		// Low poly takes over the sampling path: it already delivers hard edges, and
		// running a sharpener over its flat regions would only re-introduce the halo
		// it exists to remove. The host keeps CAS and FSR off while it is on, so the
		// three never stack.
		colour.rgb = low_poly(uv, colour.rgb, deband.y, deband.z);
	}
	else if (fsr_enable)
	{
		// FSR takes over sampling and sharpening: EASU upscales, then RCAS sharpens with
		// post.x carrying the FSR sharpness. The CAS path is never run while FSR is on, so
		// the two never stack.
		colour.rgb = fsr_easu(uv);
		if (post.x > 0.0)
			colour.rgb = fsr_rcas(uv, colour.rgb, post.x);
	}
	else if (post.x > 0.0)
	{
		colour.rgb = contrast_adaptive_sharpen(uv, colour.rgb, post.x);
	}

	// Ambient bias lighting, applied after sharpening while still in the raw sampled
	// (gamma) space and only to the colour channels: the alpha stream that carries
	// passthrough transparency below is never touched, so the wash tints the virtual
	// content near the edge without ever making transparent periphery opaque.
	if (glow.x > 0.0)
		colour.rgb = ambient_glow(colour.rgb, uv, inPosition, glow.x, glow.y);

	// Edge bleed, second half: past the packed fade distance, decay the stretched edge into
	// that edge's own averaged colour. Only the colour channels, and only in the ring, so
	// the alpha that carries passthrough transparency is untouched and a transparent
	// periphery stays transparent instead of being painted over with a smear.
	if (in_bleed && bleed_mode >= 2.0)
	{
		float d = max(bleed_t.x, bleed_t.y);
		float f = smoothstep(clamp(bleed_fade, 0.0, 0.999), 1.0, d);
		if (f > 0.0)
			colour.rgb = mix(colour.rgb, bleed_edge_colour(uv, inPosition, bleed_t), f);
	}

	// Frame smoothing. The decoded frame rate can sit far below the panel's, and each
	// decoded frame is then held for several refreshes: the picture stands still and then
	// jumps. Blending the first refresh of a new frame half and half with the frame it
	// replaces splits that jump into two, which the eye reads as a short motion blur
	// rather than a step. It is a softening of the transition, not an extra frame: no
	// motion is estimated and nothing is synthesized.
	//
	// Done here, after sharpening and the glow, in the raw sampled (gamma) space every
	// other effect in this pass works in, and on the colour channels only: the alpha
	// stream that carries passthrough transparency is never blended, so a frame's
	// transparent periphery cannot bleed into the next one's.
	if (motion.z > 0.0)
		colour.rgb = mix(colour.rgb, texture(prev_rgb, uv).rgb, motion.z);

	if (alpha == 1)
	{
		// Avoid sampling between the eyes
		vec2 a = uv_a;
		float d = a.x - 0.5;
		if (abs(d) *a_rect.z < 1)
			a.x += (d > 0 ? 1 : -1)  / float(a_rect.z);
		outColor = vec4(colour.rgb, texture(rgb[1], a).r);
	}
	else
		outColor = colour;

	if (do_srgb)
	{
		outColor = sRGB_to_linear_rgba(outColor);
	}

	if (alpha == 0)
	{
		outColor = outColor * scale + bias;
	}
	else if (outColor.a < 0.02)
	{
		outColor = vec4(0);
	}
	else
	{
		outColor.rgb /= outColor.a;
		outColor = outColor * scale + bias;
		outColor.rgb *= outColor.a;
	}

	if (post.y > 0.0)
	{
		// Soft peripheral darkening, the center of the view is left untouched
		outColor.rgb *= 1.0 - post.y * smoothstep(post.z, post.w, length(inPosition));
	}

	// Debanding, applied last, on the final stored value the panel is about to quantize.
	// The difference of two decorrelated uniform samples is triangular on [-1, 1], the
	// distribution that fully decorrelates the quantization error (constant noise power,
	// no residual contour left behind). Its peak is one 8-bit step (1/255) times the
	// strength, so a strength of 1 dithers by +/- one LSB. One dither value drives all
	// three channels: the visible banding is luminance contouring, a monochrome dither
	// erases it, and this keeps the cost to two ALU hashes with no texture taps and no
	// gradient-detection kernel (uniform dither is invisible on flat areas and
	// imperceptible on detailed ones, so the extra taps to gate it are not worth it).
	if (deband.x > 0.0)
	{
		vec2 c = gl_FragCoord.xy;
		float n = dither_noise(c) - dither_noise(c + vec2(37.0, 17.0));
		vec3 d = vec3(n * deband.x * (1.0 / 255.0));

		// Colour channels only, never the alpha that carries passthrough transparency.
		// On the alpha path the colour is premultiplied, so scaling the dither by the
		// coverage keeps a fully transparent (a == 0) periphery exactly transparent and
		// never tints the passthrough; opaque content (a == 1) gets the full dither.
		if (alpha == 1)
			outColor.rgb += d * outColor.a;
		else
			outColor.rgb += d;
	}
}

#endif
