// SPDX-License-Identifier: Apache-2.0
//
// The first pixels: one triangle in a flat colour, with no vertex buffer. Its corners are fixed
// in clip space, where y points down (Vulkan): (-0.5, -0.5) and (0.5, -0.5) along the top and
// (0, 0.5) at the bottom, at depth 0.5. It covers the centre of the target and none of its
// corners. The colour comes from a push constant, in linear RGBA.

struct PushConstants {
    float4 colour;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> push;

struct VertexOutput {
    float4 position : SV_Position;
};

static const float2 corners[3] = { float2(-0.5, -0.5), float2(0.5, -0.5), float2(0.0, 0.5) };

[shader("vertex")]
// SV_VulkanVertexID is Vulkan's VertexIndex as it is; SV_VertexID would subtract BaseVertex
// (shaderDrawParameters, which the device enables). This draw has no base vertex: both agree.
VertexOutput vertex_main(uint vertex : SV_VulkanVertexID) {
    VertexOutput output;
    output.position = float4(corners[vertex], 0.5, 1.0);
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target {
    return push.colour;
}
