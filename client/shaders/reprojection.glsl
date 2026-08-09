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

layout(set = 0, binding = 0) uniform sampler2D rgb[alpha + 1];
// One cell per motion vector, covering the whole eye image, sampled with the
// hardware bilinear filter so the warp varies smoothly across cell boundaries
layout(set = 0, binding = 1) uniform sampler2D motion_field;

layout(location = 0) in vec4 inUV;
layout(location = 1) in vec2 inPosition;

layout(location = 0) out vec4 outColor;

// AMD FidelityFX Contrast Adaptive Sharpening, single pass, no scaling, with the
// "better diagonals" soft min/max. The kernel works on the decoded image as it comes out
// of the sampler, which is still gamma encoded, that is the perceptual space CAS expects.
vec3 contrast_adaptive_sharpen(vec2 uv, float sharpness)
{
	vec2 texel = 1.0 / vec2(rgb_rect.zw);

	// a b c
	// d e f
	// g h i
	vec3 a = texture(rgb[0], uv + vec2(-1, -1) * texel).rgb;
	vec3 b = texture(rgb[0], uv + vec2(0, -1) * texel).rgb;
	vec3 c = texture(rgb[0], uv + vec2(1, -1) * texel).rgb;
	vec3 d = texture(rgb[0], uv + vec2(-1, 0) * texel).rgb;
	vec3 e = texture(rgb[0], uv).rgb;
	vec3 f = texture(rgb[0], uv + vec2(1, 0) * texel).rgb;
	vec3 g = texture(rgb[0], uv + vec2(-1, 1) * texel).rgb;
	vec3 h = texture(rgb[0], uv + vec2(0, 1) * texel).rgb;
	vec3 i = texture(rgb[0], uv + vec2(1, 1) * texel).rgb;

	// Soft min and max, the cross counts once and the whole 3x3 once, so both are in [0, 2]
	vec3 mn = min(min(min(d, e), min(f, b)), h);
	mn += min(min(min(mn, a), min(c, g)), i);

	vec3 mx = max(max(max(d, e), max(f, b)), h);
	mx += max(max(max(mx, a), max(c, g)), i);

	// Smooth distance to the signal limit divided by the smooth maximum
	vec3 amplitude = clamp(min(mn, 2.0 - mx) / max(mx, 1e-4), 0.0, 1.0);
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

	vec4 colour = texture(rgb[0], uv);
	if (post.x > 0.0)
		colour.rgb = contrast_adaptive_sharpen(uv, post.x);

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
}

#endif
