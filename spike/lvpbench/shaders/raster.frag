#version 460
// Standard-tier-like shading: every fixture evaluated per pixel (inverse square, cosine, cone).
layout(push_constant) uniform PC { mat4 view_proj; vec4 light_pos; vec4 light_dir; vec4 params; } pc;
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_nrm;
layout(location = 2) in vec3 in_alb;
layout(location = 0) out vec4 out_color;
void main() {
    vec3 n = normalize(in_nrm);
    vec3 e = vec3(0.0);
    int count = int(pc.params.y);
    for (int i = 0; i < count; ++i) {
        vec3 lp = pc.light_pos.xyz + vec3((float(i) - 0.5 * float(count - 1)) * 0.4, 0.0, 0.0);
        vec3 to_l = lp - in_pos;
        float d2 = dot(to_l, to_l);
        vec3 l = to_l * inversesqrt(d2);
        float c = dot(-l, normalize(pc.light_dir.xyz));
        float cone = smoothstep(pc.light_dir.w, pc.params.x, c);
        e += vec3(pc.light_pos.w / float(count) * cone * max(dot(n, l), 0.0) / d2);
    }
    vec3 radiance = in_alb / 3.14159265 * (e + vec3(20.0));
    out_color = vec4(radiance * pc.params.z, pc.params.z);
}
