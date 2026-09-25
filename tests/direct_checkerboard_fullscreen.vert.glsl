#version 450
layout(location = 0) out vec4 inUV;
layout(location = 1) out vec4 inPosition;
void main() {
    vec2 positions[3] = vec2[3](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
    vec2 p = positions[gl_VertexIndex];
    gl_Position = vec4(p, 0, 1);
    inUV = vec4((p + 1) * 0.5, 0, 1);
    inPosition = gl_Position;
}
