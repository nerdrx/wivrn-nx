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
	vec4 motion;
	// Ambient bias lighting
	// x: strength of the peripheral colour wash at the very edge, 0 disables it
	// y: fraction of the half image, from each edge inward, the wash covers
	vec4 glow;
	// Debanding
	// x: dither strength, in units of one 8-bit step (1/255) of triangular-PDF noise;
	//    0 disables it and the output is then byte identical to no debanding
	vec4 deband;
	// [atlas prototype] xy: the atlas picture size in samples, zw: its reciprocal.
	vec4 atlas_size;
};

#ifdef VERT_SHADER

layout (location = 0) in vec2 vPosition;
layout (location = 1) in uvec2 vUV;

layout(location = 0) out vec4 outUV;
layout(location = 1) out vec2 outPosition;

void main()
{
	gl_Position = vec4(vPosition, 0.0, 1.0);
	outPosition = vPosition;
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
layout(constant_id = 4) const bool atlas_warp = false;
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

// [atlas prototype] The atlas, laid out as ADR-0029 section 1 specifies a RefPicture:
// the whole eye picture, planes in the CODED sample domain (Y/Co/Cg, before the inverse
// colour transform), full tile extent, so the pass samples it directly with no repacking
// and does the colour transform itself. 4:2:0 here, so Y is full resolution and the
// interleaved Co/Cg plane is half.
layout(set = 0, binding = 3) uniform sampler2D atlas_y;
layout(set = 0, binding = 4) uniform sampler2D atlas_cocg;

// The per-tile table, ADR-0029 section 1: 64 bytes per tile position per eye, 289 tiles
// per eye at the v1 configuration, 37 kB for the pair -- a uniform buffer, as the ADR
// says. Bytes 0..35 are the composed homography C mapping this frame's centred sample
// coordinates to the tile's source frame; the client has already composed C with
// H(pose_t <- pose_N) in float for late latching, which is per tile and not per fragment
// and so is not in this shader.
//
// Four vec4 per tile: rows 0, 1 and 2 of C in .xyz, then src_frame/gen/flags/res_level.
layout(set = 0, binding = 5) uniform atlas_table_t
{
	vec4 e[4 * 289 * 2];
} tbl;

layout(location = 0) in vec4 inUV;
layout(location = 1) in vec2 inPosition;

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

void main()
{
	vec2 uv = inUV.xy;
	vec2 uv_a = inUV.zw;

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
	if (atlas_warp)
	{
		// Which tile this fragment lands in. Every fragment of a tile reads the same
		// three uniform vectors, which is the access pattern the constant cache is
		// for and the reason the ADR puts the table in a uniform buffer.
		vec2 t = clamp(inUV.xy, 0.0, 0.999999) * float(atlas_tiles);
		ivec2 c = ivec2(t);
		int idx = ((atlas_eye * atlas_tiles + c.y) * atlas_tiles + c.x) * 4;
		vec3 r0 = tbl.e[idx].xyz;
		vec3 r1 = tbl.e[idx + 1].xyz;
		vec3 r2 = tbl.e[idx + 2].xyz;

		// Centred sample coordinates of this fragment, through C, back to the tile's
		// source frame. One homography, one perspective divide -- ADR-0029 section 5's
		// "one step", which is the whole point of the model.
		vec2 centred = (inUV.xy - 0.5) * atlas_size.xy;
		vec3 h = vec3(centred, 1.0);
		vec3 q = vec3(dot(r0, h), dot(r1, h), dot(r2, h));
		vec2 src = q.xy / q.z;
		vec2 auv = clamp(src * atlas_size.zw + 0.5, 0.0, 1.0);

		// Two taps, bilinear by the sampler, then the inverse colour transform the
		// coded sample domain leaves to whoever reads it.
		float Y = texture(atlas_y, auv).r;
		vec2 CoCg = texture(atlas_cocg, auv).rg * 2.0 - 1.0;
		float tmp = Y - CoCg.y * 0.5;
		float G = CoCg.y + tmp;
		float B = tmp - CoCg.x * 0.5;
		float R = B + CoCg.x;
		outColor = vec4(R, G, B, 1.0);
		if (do_srgb)
			outColor = sRGB_to_linear_rgba(outColor);
		return;
	}

	vec4 colour = texture(rgb[0], uv);
	if (fsr_enable)
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
