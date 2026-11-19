// SPDX-License-Identifier: Apache-2.0
//
// For tests only (libs/gpu/tests/test_tonemap.cpp): evaluates colour.slang's functions on inputs
// from buffers, so that they can be compared with libs/core's. No pipeline of the renderer is made
// from it.

import colour;

struct ColourCheckPush {
    float4* colours;      // xyz: exposed linear RGB, input to pbr_neutral
    float4* tone_mapped;  // written: xyz pbr_neutral(colours[i].xyz), w 0
    float* ev100;         // input to exposure_scale
    float* scales;        // written: exposure_scale(ev100[i])
    uint colour_count;
    uint ev100_count;
};

[shader("compute")]
[numthreads(64, 1, 1)]
void check_colour(uint3 id : SV_DispatchThreadID, uniform ColourCheckPush push) {
    if (id.x < push.colour_count)
        push.tone_mapped[id.x] = float4(pbr_neutral(push.colours[id.x].xyz), 0.0);
    if (id.x < push.ev100_count)
        push.scales[id.x] = exposure_scale(push.ev100[id.x]);
}
