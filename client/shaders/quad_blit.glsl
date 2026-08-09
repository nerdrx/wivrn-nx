/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Patrick Nicolas <patricknicolas@laposte.net>
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

// Promoted quad layer: one decoded picture into the swapchain the runtime is handed
// as a composition layer. No foveation grid, no reprojection, no scale or bias: what
// the server encoded is what the panel shows, one texel per pixel where the sizes
// agree.

#version 450

layout(push_constant) uniform quad_pc
{
	// Part of the decoded image that holds picture, in normalized coordinates
	vec4 uv_rect;
};

#ifdef VERT_SHADER

layout(location = 0) out vec2 outUV;

void main()
{
	// Full screen triangle, no vertex buffer
	vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
	outUV = uv_rect.xy + p * uv_rect.zw;
}
#endif

#ifdef FRAG_SHADER

layout(constant_id = 0) const bool do_srgb = false;

layout(set = 0, binding = 0) uniform sampler2D source;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

float sRGB_to_linear(float x)
{
	if (x <= 0.04045)
		return x / 12.92;
	return pow((x + 0.055) / 1.055, 2.4);
}

void main()
{
	vec3 colour = texture(source, inUV).rgb;

	if (do_srgb)
		colour = vec3(
		        sRGB_to_linear(colour.r),
		        sRGB_to_linear(colour.g),
		        sRGB_to_linear(colour.b));

	// The stream carries no alpha: only layers the application marked opaque are
	// ever promoted, so the panel is submitted opaque.
	outColor = vec4(colour, 1.0);
}

#endif
