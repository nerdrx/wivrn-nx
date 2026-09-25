#version 450

layout(push_constant) uniform pc {
    ivec4 rgb_rect;
    ivec4 a_rect;
    vec4 scale;
    vec4 bias;
    vec4 post;
    vec4 motion;
    vec4 glow;
    vec4 deband;
};
layout(constant_id = 0) const int alpha = 1;
layout(constant_id = 1) const bool do_srgb = false;
layout(set = 0, binding = 0) uniform sampler2D rgb[alpha + 1];
layout(set = 0, binding = 3, std430) readonly buffer direct_tiles_t { uint tile[]; } tiles;
layout(set = 0, binding = 4, std430) readonly buffer direct_blocks_t { uint block[]; } blocks;
layout(set = 0, binding = 5, std430) readonly buffer direct_dummy_t { uint dummy[]; } direct_meta;
layout(location = 0) in vec4 inUV;
layout(location = 1) in vec4 inPosition;
layout(location = 0) out vec4 outColor;

uvec3 rgb565(uint v) {
    uvec3 c = uvec3((v >> 11) & 31u, (v >> 5) & 63u, v & 31u);
    return uvec3((c.r << 3) | (c.r >> 2), (c.g << 2) | (c.g >> 4), (c.b << 3) | (c.b >> 2));
}
vec3 srgb_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
uint direct_tile_id(uvec2 p) {
    uint cols = (uint(max(motion.x, 1.0)) + 31u) / 32u;
	return (p.y / 32u) * cols + p.x / 32u;
}
uint checker_code(bool previous) {
	return previous ? uint(glow.w) - 1u : uint(glow.z);
}
bool frame_has_sample(uint code, uvec2 p, uint shift) {
	return code == 0u || (((p.x >> shift) ^ (p.y >> shift)) & 1u) == code - 1u;
}
vec3 sample_block(uvec2 p, uint d, uint frame_code, uint base, bool nearest_missing) {
	uint mode = d >> 30u;
	if ((d & 0x20000000u) != 0u) {
		uint offset = d & 0x0fffffffu;
		uvec2 local = p % 32u;
		local.x ^= nearest_missing ? 1u : 0u;
        uint index;
        if (frame_code != 0u) {
            index = local.y * 16u + local.x / 2u;
        } else {
            index = local.y * 32u + local.x;
        }
        offset += base;
        if ((d & 0x10000000u) != 0u) {
            uint pair = blocks.block[offset + index / 2u];
            return vec3(rgb565((pair >> (16u * (index % 2u))) & 65535u)) / 255.0;
        }
		uint rgb = blocks.block[offset + index];
		return vec3((rgb >> 16u) & 0xffu, (rgb >> 8u) & 0xffu, rgb & 0xffu) / 255.0;
	}
    if (mode == 3u)
        return vec3((d >> 16u) & 0xffu, (d >> 8u) & 0xffu, d & 0xffu) / 255.0;
	uint shift = mode;
	uvec2 q = (p % 32u) >> shift;
	q.x ^= nearest_missing ? 1u : 0u;
    uint blocks_per_row = 4u >> shift;
	uint stride = frame_code == 0u ? 5u : 3u;
	uint offset = base + (d & 0x3fffffffu) + stride * ((q.y / 8u) * blocks_per_row + q.x / 8u);
    uint endpoints = blocks.block[offset];
    uint index;
    if (frame_code != 0u) {
        uint local_x = q.x % 8u, local_y = q.y % 8u;
        index = local_y * 4u + local_x / 2u;
    } else {
        index = (q.y % 8u) * 8u + q.x % 8u;
    }
    uint selector = (blocks.block[offset + 1u + index / 16u] >> (2u * (index % 16u))) & 3u;
    uvec3 c = ((3u - selector) * rgb565(endpoints & 65535u) + selector * rgb565(endpoints >> 16u) + 1u) / 3u;
    return vec3(c) / 255.0;
}
vec3 sample_direct(uvec2 p) {
    uint tile_id = direct_tile_id(p);
    uint d = tiles.tile[tile_id];
    // Uniform per stream; keep full-rate sampling on its original cheap path.
    if (glow.x < 0.5)
        return sample_block(p, d, 0u, 0u, false);
    uint code = checker_code(false);
    uint mode = d >> 30u;
    uint shift = ((d & 0x20000000u) != 0u || mode == 3u) ? 0u : mode;
    bool missing = code != 0u && mode != 3u && !frame_has_sample(code, p, shift);
    uint base = 0u;
    // Adjacent pixels choose different frames. Select their addressing first,
    // then decode once so a SIMD group does not execute both sampling paths.
    if (glow.w >= 1.0) {
        uint old_d = tiles.tile[uint(glow.x) + tile_id];
        uint old_code = checker_code(true);
        bool same_mode = (d >> 30u) == (old_d >> 30u) &&
                         ((d ^ old_d) & 0x30000000u) == 0u;
        bool previous = missing && same_mode && frame_has_sample(old_code, p, shift);
        d = previous ? old_d : d;
        code = previous ? old_code : code;
        base = previous ? uint(glow.y) : 0u;
        missing = missing && !previous;
    }
    return sample_block(p, d, code, base, missing);
}

void main() {
    vec2 uv = clamp(inUV.xy, vec2(0), vec2(0.999999));
	vec3 c = sample_direct(uvec2(uv * motion.xy));
    if (do_srgb) c = srgb_linear(c);
    outColor = vec4(c * scale.rgb + bias.rgb, 1.0);
}
