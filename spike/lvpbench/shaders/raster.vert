#version 460
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_nrm;
layout(location = 2) in vec3 in_alb;
layout(push_constant) uniform PC { mat4 view_proj; vec4 light_pos; vec4 light_dir; vec4 params; } pc;
layout(location = 0) out vec3 out_pos;
layout(location = 1) out vec3 out_nrm;
layout(location = 2) out vec3 out_alb;
void main() {
    out_pos = in_pos; out_nrm = in_nrm; out_alb = in_alb;
    gl_Position = pc.view_proj * vec4(in_pos, 1.0);
}
