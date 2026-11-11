// SPDX-License-Identifier: Apache-2.0
#version 450
// A triangle with hard-coded corners, rotated by the push constant; no vertex buffer.
layout(push_constant) uniform Push {
    float angle_rad;
    float aspect;    // width / height of the target, so the triangle keeps its shape
    float offset_y;  // pitch from relative mouse motion in walk mode
    float pad;
} pc;
layout(location = 0) out vec3 v_colour;

const vec2 corners[3] = vec2[](vec2(0.0, -0.6), vec2(0.52, 0.3), vec2(-0.52, 0.3));
const vec3 colours[3] = vec3[](vec3(1.0, 0.1, 0.1), vec3(0.1, 1.0, 0.1), vec3(0.1, 0.2, 1.0));

void main() {
    vec2 p = corners[gl_VertexIndex];
    float c = cos(pc.angle_rad), s = sin(pc.angle_rad);
    p = vec2(c * p.x - s * p.y, s * p.x + c * p.y);
    gl_Position = vec4(p.x / pc.aspect, p.y + pc.offset_y, 0.0, 1.0);
    v_colour = colours[gl_VertexIndex];
}
