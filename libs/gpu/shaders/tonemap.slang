// SPDX-License-Identifier: Apache-2.0
//
// The tone-mapping pass (libs/gpu/include/cs/gpu/tonemap.hpp): one triangle over the whole
// target, and for each pixel the HDR texel under it, exposed and tone mapped by colour.slang. The
// target's _SRGB format encodes what the fragment shader returns.

import colour;

// RGB radiance in nits, A illuminance in lux. Read with Load, one texel per pixel: RGBA32F need
// not be linearly filterable, and the pixel grids are the same.
[[vk::binding(0, 0)]] Texture2D<float4> hdr;

struct ToneMapPush {
    float ev100;
};

// Corners (-1, -1), (3, -1) and (-1, 3): a triangle that covers the clip square.
[shader("vertex")]
float4 vertex_main(uint vertex : SV_VulkanVertexID) : SV_Position {
    const float2 corner = float2((vertex << 1) & 2, vertex & 2);
    return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}

[shader("fragment")]
float4 fragment_main(float4 pixel : SV_Position, uniform ToneMapPush push) : SV_Target {
    // Only RGB: the illuminance in A is no alpha, and the display image is opaque.
    const float3 nits = hdr.Load(int3(int2(pixel.xy), 0)).rgb;
    return float4(pbr_neutral(nits * exposure_scale(push.ev100)), 1.0);
}
