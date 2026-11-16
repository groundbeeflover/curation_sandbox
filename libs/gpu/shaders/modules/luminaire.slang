// SPDX-License-Identifier: Apache-2.0
//
// A luminaire as the Standard tier reads it: cs::gpu::LightData and its profile table
// (libs/gpu/include/cs/gpu/standard_tier.hpp, which says how the table is laid out and why).
// Everything here is in the world's units: metres and candela.

module luminaire;

// The table's two parts, as profile_core_entries and profile_taper_entries.
public static const uint profile_core_entries = 1024;
public static const uint profile_taper_entries = 256;

// cs::gpu::LightData, 80 bytes; its float3 + scalar pairs have the same offsets in Slang's
// natural layout as in C++.
public struct LightData {
    public float3 position_m;
    public float peak_cd;
    public float3 axis_x;
    public float core_end; // sin(theta / 2) where the core of the table ends
    public float3 axis_y;
    public float lit_end; // sin(theta / 2) beyond which nothing is emitted
    public float3 axis_z;
    public uint profile; // the table's first entry in the profiles
    public float3 colour; // linear Rec. 709 of luminance 1
    public uint padding;
};

// The profiles follow the lights in the same buffer (cs::gpu::LightTable).
public float* light_profiles(LightData* lights, uint light_count) {
    return (float*)(lights + light_count);
}

// The table at a position between 0 and count - 1, interpolated linearly.
float interpolate(float* table, float position, uint count) {
    const float clamped = clamp(position, 0.0, float(count - 1));
    const uint below = min(uint(clamped), count - 2);
    return lerp(table[below], table[below + 1], clamped - float(below));
}

// The light's intensity in candela towards a world direction, which need not be of unit length
// but must not be zero.
public float intensity_cd(LightData light, float* profiles, float3 world_direction) {
    const float3 d = normalize(world_direction);
    const float x = dot(d, light.axis_x);
    const float y = dot(d, light.axis_y);
    const float z = dot(d, light.axis_z);
    // 1 - cos(theta) without cancelling near the axis, and w = sin(theta / 2): the table's index.
    const float one_minus_cos = z > 0.0 ? (x * x + y * y) / (1.0 + z) : 1.0 - z;
    const float w = sqrt(0.5 * one_minus_cos);
    if (!(w <= light.lit_end))
        return 0.0;
    float* table = profiles + light.profile;
    if (w <= light.core_end) {
        // w = core_end * s^2, s evenly spaced over the core's entries.
        const float s = sqrt(w / light.core_end);
        return light.peak_cd * interpolate(table, s * float(profile_core_entries - 1),
                                           profile_core_entries);
    }
    const float t = (w - light.core_end) / (light.lit_end - light.core_end);
    return light.peak_cd * interpolate(table + profile_core_entries,
                                       t * float(profile_taper_entries - 1), profile_taper_entries);
}
