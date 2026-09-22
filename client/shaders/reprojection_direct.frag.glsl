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
vec3 sample_block(uvec2 p) {
    uint cols = (uint(max(rgb_rect.z, 1)) + 31u) / 32u;
    uint d = tiles.tile[(p.y / 32u) * cols + p.x / 32u];
    uint mode = d >> 30u;
    if (mode == 3u)
        return vec3((d >> 16u) & 0xffu, (d >> 8u) & 0xffu, d & 0xffu) / 255.0;
    uint shift = mode;
    uvec2 q = (p % 32u) >> shift;
    uint blocks_per_row = 4u >> shift;
    uint offset = (d & 0x3fffffffu) + 5u * ((q.y / 8u) * blocks_per_row + q.x / 8u);
    uint endpoints = blocks.block[offset];
    uint index = (q.y % 8u) * 8u + q.x % 8u;
    uint selector = (blocks.block[offset + 1u + index / 16u] >> (2u * (index % 16u))) & 3u;
    uvec3 c = ((3u - selector) * rgb565(endpoints & 65535u) + selector * rgb565(endpoints >> 16u) + 1u) / 3u;
    return vec3(c) / 255.0;
}
void main() {
    vec2 uv = clamp(inUV.xy, vec2(0), vec2(0.999999));
    vec3 c = sample_block(uvec2(uv * vec2(rgb_rect.zw)));
    if (do_srgb) c = srgb_linear(c);
    outColor = vec4(c * scale.rgb + bias.rgb, 1.0);
}
