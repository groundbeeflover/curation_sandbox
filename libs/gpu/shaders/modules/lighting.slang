// SPDX-License-Identifier: Apache-2.0
//
// Direct light from point luminaires, without shadows: what the Standard tier evaluates exactly
// at every pixel (libs/gpu/include/cs/gpu/standard_tier.hpp).

module lighting;

import luminaire;

// The light arriving at a surface point from every luminaire.
public struct DirectLight {
    public float3 coloured_lux; // the sum of colour * E: radiance is albedo / pi times this
    public float lux;           // the illuminance E, in lux
};

// For each light: E = I * cos(theta) / d^2, with theta between the unit normal and the direction
// to the light; a light behind the surface, or at the point itself, adds nothing.
public DirectLight direct_light(float3 point_m, float3 normal, LightData* lights,
                                uint light_count) {
    float* profiles = light_profiles(lights, light_count);
    DirectLight result = { float3(0.0), 0.0 };
    for (uint i = 0; i < light_count; ++i) {
        const LightData light = lights[i];
        const float3 to_light = light.position_m - point_m;
        const float d2 = dot(to_light, to_light);
        if (!(d2 > 0.0))
            continue;
        const float3 wi = to_light * rsqrt(d2);
        const float cos_theta = dot(normal, wi);
        if (!(cos_theta > 0.0))
            continue;
        const float lux = intensity_cd(light, profiles, -wi) * cos_theta / d2;
        result.coloured_lux += light.colour * lux;
        result.lux += lux;
    }
    return result;
}
