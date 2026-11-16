// SPDX-License-Identifier: Apache-2.0
//
// A test fixture for the device's shaderDrawParameters feature (device.hpp): SV_VertexID counts
// from the draw's first vertex (Slang compiles it as VertexIndex minus BaseVertex), so the scene's
// meshes, drawn with their first vertex as the base, index their own vertices from 0.
//
// Vertices 0, 1 and 2 are triangle.slang's triangle: (-0.5, -0.5) and (0.5, -0.5) along the top
// and (0, 0.5) at the bottom of clip space, at depth 0.5, covering the centre of the target. Any
// other vertex lies outside the clip volume, so a draw that sees VertexIndex itself draws nothing.
// The colour comes from a push constant, in linear RGBA.

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
VertexOutput vertex_main(uint vertex : SV_VertexID) {
    VertexOutput output;
    if (vertex < 3)
        output.position = float4(corners[vertex], 0.5, 1.0);
    else
        output.position = float4(4.0, 4.0, 0.5, 1.0);
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target {
    return push.colour;
}
