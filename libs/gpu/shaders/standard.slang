// SPDX-License-Identifier: Apache-2.0
//
// The Standard tier's forward pass (libs/gpu/include/cs/gpu/standard_tier.hpp): every instance of
// the scene, lit exactly by every luminaire, without shadows, into one HDR target whose RGB is
// radiance in nits and whose A is illuminance in lux.
//
// There is no vertex input: the vertex shader reads the scene's buffers through their device
// addresses (cs/gpu/scene.hpp). Each instance is one indexed draw whose vertexOffset is its mesh's
// first vertex and whose firstInstance is its index, so SV_VulkanVertexID (VertexIndex, base
// included) is the vertex in the whole buffer and SV_VulkanInstanceID the instance.
//
// check_intensity runs luminaire.slang's intensity_cd on directions from a buffer, so that tests
// can compare the shader's profile with light::GenericSpot (libs/gpu/tests/test_standard_tier.cpp).

import lighting;
import luminaire;

// ---- The scene, as cs/gpu/scene.hpp lays it out -------------------------------------------------
// Written with scalars and float4s only, so that every offset is the C++ one under any layout
// rule: Vertex's two float3s would not be, under std430.

struct Vertex {
    float px, py, pz; // position_m
    float nx, ny, nz; // normal
    float u, v;
};

struct InstanceData {
    float4 transform[4]; // columns: the mesh's coordinates to the world
    uint mesh;
    uint material;
    uint first_index;
    uint vertex_offset;
};

static const uint material_painting = 1; // MaterialKind::painting
static const uint no_texture = 0xFFFFFFFFu;

struct MaterialData {
    float3 base_colour;
    uint kind;
    uint texture;
    float roughness;
    float specular_weight;
    float coat_weight;
    float coat_roughness;
    float coat_ior;
    uint padding[2];
};

// A painting's texture, or a white texel for every other material (it is not read there), and
// the tier's one sampler: trilinear, clamped to the edge.
[[vk::binding(0, 0)]] Texture2D<float4> albedo_texture;
[[vk::binding(1, 0)]] SamplerState albedo_sampler;

// cs::gpu's push constants for this pass: 120 bytes.
struct ForwardPush {
    float4 view_projection[4]; // columns: world to clip space
    // Towards the viewer from a point p is eye.xyz - p * eye.w: the camera's position with w = 1
    // for a perspective camera, minus its forward with w = 0 for an orthographic one.
    float4 eye;
    Vertex* vertices;
    InstanceData* instances;
    MaterialData* materials;
    LightData* lights; // then the profiles
    uint light_count;
    uint padding;
};

struct Varyings {
    float4 clip : SV_Position;
    float3 position_m : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
    nointerpolation uint material : MATERIAL;
};

[shader("vertex")]
Varyings vertex_main(uint vertex : SV_VulkanVertexID, uint instance : SV_VulkanInstanceID,
                     uniform ForwardPush push) {
    const InstanceData object = push.instances[instance];
    const Vertex v = push.vertices[vertex];
    const float3 position_m = (object.transform[0] * v.px + object.transform[1] * v.py +
                               object.transform[2] * v.pz + object.transform[3]).xyz;
    // The transforms are rotations and translations, so the upper 3x3 turns normals too.
    const float3 normal = object.transform[0].xyz * v.nx + object.transform[1].xyz * v.ny +
                          object.transform[2].xyz * v.nz;
    Varyings output;
    output.clip = push.view_projection[0] * position_m.x + push.view_projection[1] * position_m.y +
                  push.view_projection[2] * position_m.z + push.view_projection[3];
    output.position_m = position_m;
    output.normal = normal;
    output.uv = float2(v.u, v.v);
    output.material = object.material;
    return output;
}

// [0, 1], with NaN as 0, as the engine clamps albedos (engine/textures.hpp).
float3 clamp_albedo(float3 albedo) {
    return float3(albedo.r > 0.0 ? min(albedo.r, 1.0) : 0.0,
                  albedo.g > 0.0 ? min(albedo.g, 1.0) : 0.0,
                  albedo.b > 0.0 ? min(albedo.b, 1.0) : 0.0);
}

[shader("fragment")]
float4 fragment_main(Varyings input, uniform ForwardPush push) : SV_Target {
    const MaterialData material = push.materials[input.material];
    // Sampled for every material, so that the derivatives are taken in uniform control flow.
    const float3 texel = albedo_texture.Sample(albedo_sampler, input.uv).rgb;
    const bool textured = material.kind == material_painting && material.texture != no_texture;
    const float3 albedo = clamp_albedo(textured ? texel : material.base_colour);

    // Both sides reflect: the normal faces the viewer.
    float3 normal = normalize(input.normal);
    if (dot(normal, push.eye.xyz - input.position_m * push.eye.w) < 0.0)
        normal = -normal;

    const DirectLight light = direct_light(input.position_m, normal, push.lights, push.light_count);
    return float4(albedo * (1.0 / 3.14159265358979) * light.coloured_lux, light.lux);
}

// ---- For tests ----------------------------------------------------------------------------------

struct CheckPush {
    LightData* lights; // a LightTable: the lights, then the profiles
    float3* directions; // world directions from the light, of any non-zero length
    float* intensities; // written: intensity_cd(lights[light], direction), in candela
    uint light_count;
    uint light;
    uint count; // directions
    uint padding;
};

[shader("compute")]
[numthreads(64, 1, 1)]
void check_intensity(uint3 id : SV_DispatchThreadID, uniform CheckPush push) {
    if (id.x >= push.count)
        return;
    push.intensities[id.x] = intensity_cd(push.lights[push.light],
                                          light_profiles(push.lights, push.light_count),
                                          push.directions[id.x]);
}
