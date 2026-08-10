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

// Custom fragment shader for the NX space-lobby sky dome (equirect emissive
// starfield). Used in place of lit.frag when either the "Animated lobby" or the
// "Warp transition" client toggle is enabled. With both effects idle
// (anim_params.z == 0 and anim_params.y == 0) it computes exactly the same
// expression as lit.frag's non-PBR path, so the static scene is unchanged.

#version 450

#extension GL_GOOGLE_include_directive : require

// Pull the extra per-node uniform (anim_params) into mesh_ubo, see scene_renderer
// which appends components::node::extra_shader_data right after the instance data.
#define SKY_ANIM_PARAMS

#include "common.glsl.inc"

// Fragment output
layout(location = 0) out vec4 out_color;
layout(early_fragment_tests) in;

// anim_params: x = elapsed time (s, wrapped), y = warp amount [0,1],
//              z = animated-lobby enabled (0/1), w = unused
#define ANIM_TIME (mesh.anim_params.x)
#define ANIM_WARP (mesh.anim_params.y)
#define ANIM_ON   (mesh.anim_params.z)

// Gas giant location in the equirect texture (see assets-source/ground.glb,
// space_sky 4096x2048). Distances use a factor 2 on u because an equirect map
// is twice as wide as it is tall.
const vec2 planet_center = vec2(0.153, 0.480);
const float planet_radius = 0.055; // in "v" units
const float planet_rate = 0.10;    // rad/s, slow self-rotation

// Cheap hash for per-star twinkle phase.
float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float luminance(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// Low-amplitude, spatially varied brightness shimmer applied to bright texels
// (stars) only, so the nebula does not pulse. OLED-friendly: no hard flashing.
vec3 apply_twinkle(vec3 color, vec2 uv, float time)
{
    float lum = luminance(color);
    // Only stars (bright, small) twinkle; ramp in above the nebula level.
    float star = smoothstep(0.35, 0.75, lum);
    if (star <= 0.0)
        return color;

    vec2 cell = floor(uv * vec2(4096.0, 2048.0) / 3.0);
    float phase = hash12(cell) * 6.2831853;
    float shimmer = sin(time * 2.4 + phase);
    return color * (1.0 + 0.16 * star * shimmer);
}

// A faint violet-tailed comet streaking across the dome. Deterministic: one
// pass every `period` seconds, path derived from the cycle index so successive
// comets differ without any RNG.
vec3 apply_comet(vec3 color, vec2 uv)
{
    const float period = 52.0; // seconds between comets
    const float travel = 6.0;  // seconds of visible travel
    float t = ANIM_TIME;
    float cycle = floor(t / period);
    float local = t - cycle * period;
    if (local > travel)
        return color;

    // Per-cycle path parameters (deterministic).
    float y0 = 0.18 + 0.5 * hash12(vec2(cycle, 1.0));
    float y1 = y0 + (hash12(vec2(cycle, 2.0)) - 0.5) * 0.25;
    float dir = hash12(vec2(cycle, 3.0)) < 0.5 ? 1.0 : -1.0;

    float p = local / travel;                 // 0..1 progress
    float hx = dir > 0.0 ? (p * 1.2 - 0.1) : (1.1 - p * 1.2);
    float hy = mix(y0, y1, p);
    vec2 head = vec2(hx, hy);

    // Fade in and out over the pass.
    float life = smoothstep(0.0, 0.12, p) * (1.0 - smoothstep(0.75, 1.0, p));

    // Distance to head and to the tail behind it (tail trails opposite motion).
    vec2 d = vec2((uv.x - head.x) * 2.0, uv.y - head.y);
    vec2 motion = normalize(vec2(dir * 2.0, (y1 - y0)));
    float along = dot(d, motion);   // >0 ahead of head, <0 behind (tail)
    float across = length(d - along * motion);

    float head_glow = exp(-across * across * 5000.0) * exp(-max(along, 0.0) * 220.0);
    float tail = exp(-across * across * 9000.0) * exp(max(along, 0.0) * 60.0) * smoothstep(-0.20, 0.0, along);
    float intensity = (head_glow + 0.6 * tail) * life;

    // Violet, matching the #7700FF NX identity.
    vec3 comet_color = vec3(0.47, 0.0, 1.0);
    return color + comet_color * intensity * 0.9;
}

// Very subtle self-rotation of the (featureless) gas giant: a slow longitudinal
// brightness wave confined to the planet body disc, faded at the limb so the
// rings (which sit outside the body radius) are untouched. Reads as rotation
// without separating the baked planet from the equirect.
vec3 apply_planet(vec3 color, vec2 uv, float time)
{
    vec2 pd = vec2((uv.x - planet_center.x) * 2.0, uv.y - planet_center.y);
    float r = length(pd) / planet_radius;
    if (r >= 1.0)
        return color;

    float lon = asin(clamp(pd.x / planet_radius, -1.0, 1.0));
    float bands = sin(pd.y / planet_radius * 6.2831853 * 1.5);
    float rot = sin(lon * 2.0 + time * planet_rate + bands * 0.5);
    float limb = smoothstep(1.0, 0.55, r);
    return color * (1.0 + 0.06 * rot * limb);
}

// Hyperspace warp: stretch the starfield into radial streaks rushing outward
// from the view centre. Screen-radial direction comes from the view-space
// fragment position; the emissive texture is resampled a few times along that
// direction in UV space (via screen-space derivatives) to build the streak.
vec3 apply_warp(vec2 uv, float warp, float time)
{
    // Screen-radial direction (outward from centre) in view space.
    vec2 radial = frag_pos.xy;
    float rl = length(radial);
    if (rl < 1e-4)
        return (material.base_emissive_factor * texture(emissive_texture, uv)).rgb;
    radial /= rl;

    // Map the screen-radial direction into UV space using derivatives.
    vec2 duv = dFdx(uv) * radial.x + dFdy(uv) * radial.y;
    float dl = length(duv);
    if (dl > 1e-6)
        duv /= dl;

    // Streak length grows with warp; add an outward rush over time.
    float len = warp * 0.06;
    float rush = fract(time * 0.7) * warp * 0.02;

    const int N = 6;
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    for (int i = 0; i < N; ++i)
    {
        float f = float(i) / float(N - 1);          // 0..1 along the streak
        float w = 1.0 - f * 0.65;                    // fade toward the tail
        vec2 s = uv - duv * (f * len + rush);
        acc += (material.base_emissive_factor * texture(emissive_texture, s)).rgb * w;
        wsum += w;
    }
    vec3 streak = acc / wsum;
    // Brighten the streaks as the tunnel builds.
    return streak * (1.0 + warp * 1.4);
}

void main()
{
    // --- Base emissive path, identical to lit.frag (non-PBR) for the sky. ---
    vec4 albedo = vertex_color * material.base_color_factor * texture(base_color_texture, texcoord_base_color);
    vec3 emissive_color = (material.base_emissive_factor * texture(emissive_texture, texcoord_emissive)).rgb;

    vec3 normal_unit = normalize(normal);
    vec3 light_dir = normalize(light_pos.xyz - frag_pos.xyz * light_pos.w);

    vec3 base = emissive_color + albedo.rgb * (max(0, dot(normal_unit, light_dir)) * scene.light_color.rgb + scene.ambient_color.rgb);

    // Idle: no effects -> byte-identical to lit.frag.
    vec3 color = base;

    float warp = ANIM_WARP;

    if (ANIM_ON > 0.5)
    {
        color = apply_planet(color, texcoord_emissive, ANIM_TIME);
        color = apply_twinkle(color, texcoord_emissive, ANIM_TIME);
        color = apply_comet(color, texcoord_emissive);
    }

    if (warp > 0.001)
    {
        vec3 warped = apply_warp(texcoord_emissive, warp, ANIM_TIME);
        color = mix(color, warped, warp);
    }

    out_color = vec4(color, albedo.a);
}
