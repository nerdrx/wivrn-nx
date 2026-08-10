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
	vec4 motion;
	// Ambient bias lighting
	// x: strength of the peripheral colour wash at the very edge, 0 disables it
	// y: fraction of the half image, from each edge inward, the wash covers
	vec4 glow;
	// Debanding
	// x: dither strength, in units of one 8-bit step (1/255) of triangular-PDF noise;
	//    0 disables it and the output is then byte identical to no debanding
	vec4 deband;
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

layout(set = 0, binding = 0) uniform sampler2D rgb[alpha + 1];
// One cell per motion vector, covering the whole eye image, sampled with the
// hardware bilinear filter so the warp varies smoothly across cell boundaries
layout(set = 0, binding = 1) uniform sampler2D motion_field;

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
	// opaque pixel pays a single centre sample whether or not it is sharpened.
	vec4 colour = texture(rgb[0], uv);
	if (post.x > 0.0)
		colour.rgb = contrast_adaptive_sharpen(uv, colour.rgb, post.x);

	// Ambient bias lighting, applied after sharpening while still in the raw sampled
	// (gamma) space and only to the colour channels: the alpha stream that carries
	// passthrough transparency below is never touched, so the wash tints the virtual
	// content near the edge without ever making transparent periphery opaque.
	if (glow.x > 0.0)
		colour.rgb = ambient_glow(colour.rgb, uv, inPosition, glow.x, glow.y);

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
