// SPDX-License-Identifier: Apache-2.0
//
// Exposure and tone mapping, ported from libs/core/include/cs/core/colour.hpp, which is the one
// definition and says why: linear RGB in nits, times exposure_scale(ev100), through pbr_neutral,
// gives display-referred linear RGB in [0, 1], which an _SRGB target then encodes. In float here,
// in double there; libs/gpu/tests/test_tonemap.cpp holds the two within 1e-4.

module colour;

// The camera behind EV100 (Lagarde and de Rousiers, 2014, section 4.5): ISO 100, a meter
// calibrated with K = 12.5, and a sensor that saturates at 1.2 * 2^EV100 nits.
public static const float ev100_iso = 100.0;
public static const float ev100_meter_calibration = 12.5;
public static const float ev100_saturation_factor = 78.0 / (ev100_iso * 0.65);

// What to multiply linear RGB in nits by before tone mapping: 1 / (1.2 * 2^EV100).
public float exposure_scale(float ev100) {
    return 1.0 / (ev100_saturation_factor * exp2(ev100));
}

// c if it is above 0, else 0, NaN included, as libs/core's max(0, c). Decided on the bits, as
// integers: without SignedZeroInfNanPreserve, Vulkan lets a driver assume that no float is NaN,
// so a float comparison might let one through. The floats above 0, +inf included, are the bit
// patterns 1 to 0x7f800000; -0, negatives and NaN wrap or lie beyond.
float positive_or_zero(float c) {
    return asuint(c) - 1u < 0x7f800000u ? c : 0.0;
}

// Khronos PBR Neutral tone mapping (github.com/KhronosGroup/ToneMapping, version of 2024), with
// negative and NaN channels taken as 0.
public float3 pbr_neutral(float3 rgb) {
    const float start_compression = 0.8 - 0.04;
    const float desaturation = 0.15;

    rgb = float3(positive_or_zero(rgb.r), positive_or_zero(rgb.g), positive_or_zero(rgb.b));

    const float x = min(rgb.r, min(rgb.g, rgb.b));
    const float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    rgb -= offset;

    const float peak = max(rgb.r, max(rgb.g, rgb.b));
    if (peak < start_compression)
        return rgb;

    const float d = 1.0 - start_compression;
    const float new_peak = 1.0 - d * d / (peak + d - start_compression);
    rgb *= new_peak / peak;

    const float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return rgb * (1.0 - g) + float3(new_peak) * g;
}
