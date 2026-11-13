// SPDX-License-Identifier: Apache-2.0

#pragma once

// Colour, exposure and tone mapping
// ---------------------------------
// Every renderer works in linear RGB with Rec. 709 (sRGB) primaries, and the luminance Y of a
// colour is in nits (cd/m^2). A frame goes to the screen in three steps:
//
//     linear RGB in nits  --exposure_scale(ev100)-->  exposed linear RGB, about [0, 1]
//                         --pbr_neutral-->            display-referred linear RGB in [0, 1]
//                         --linear_to_srgb8-->        8-bit sRGB (image.hpp)
//
// The exposure is that of a physical camera (EV100), set by the viewpoint or found from the
// frame's average luminance. Khronos PBR Neutral tone mapping leaves colours alone below about
// 0.76 and rolls the highlights off to white, so an artwork keeps its hues (ADR 0002, Colour and
// exposure). Lux is never read from these pixels; the engine calculates it.

#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>

namespace cs {

// The Rec. 709 weights of Y in linear RGB, the middle row of the RGB to XYZ matrix. For a colour
// in this project's convention the result is its luminance in nits. It reads x, y and z rather
// than r, g and b because x is glm's active union member, the only one a constant expression
// may read.
template <typename T> constexpr T luminance(const glm::vec<3, T>& rgb) {
    return T(0.2126) * rgb.x + T(0.7152) * rgb.y + T(0.0722) * rgb.z;
}

// The colour of a blackbody (a Planckian radiator) at kelvin, in linear Rec. 709 RGB with
// luminance 1: multiply by a luminance or intensity to light with it. 2700 K is a warm
// incandescent orange, 6500 K is close to white but slightly magenta of D65, with green the
// smallest channel (a blackbody is not a daylight illuminant), 25000 K is a pale blue.
//
// The chromaticity comes from Kim, Kim, Lee, Kim and Kim's cubic fit of the Planckian locus in
// CIE 1931 xy (US patent 7,024,034, 2006), within delta-uv 0.0004 of the locus from 1667 K to
// 25000 K; kelvin is clamped to that range, and NaN gives 1667 K. Below about 1900 K the locus
// lies outside the Rec. 709 gamut, so a negative blue is clipped to zero before normalising:
// light is never negative, and the colour stays continuous in temperature.
glm::dvec3 cct_to_linear_rgb(double kelvin);

// The range cct_to_linear_rgb follows the locus over.
inline constexpr double cct_min_kelvin = 1667.0;
inline constexpr double cct_max_kelvin = 25000.0;

// The camera behind EV100 (Lagarde and de Rousiers, Moving Frostbite to PBR, 2014, section 4.5):
// ISO 100, a reflected-light meter calibrated with K = 12.5, and a sensor that saturates at
// 78 / (S q) * 2^EV100 = 1.2 * 2^EV100 nits, with lens and vignetting factor q = 0.65.
inline constexpr double ev100_iso = 100.0;
inline constexpr double ev100_meter_calibration = 12.5;
inline constexpr double ev100_saturation_factor = 78.0 / (ev100_iso * 0.65);

// The darkest average luminance ev100_from_average_luminance meters, about a moonless night sky
// (EV100 -10.3). A black frame, an empty room with the lights off, thus gets a finite exposure.
inline constexpr double ev100_min_luminance_nits = 1e-4;

// The exposure, as EV100, at which the light meter puts an average scene luminance in nits at
// middle grey: EV100 = log2(L * S / K). Luminances below ev100_min_luminance_nits, and NaN, meter
// as that minimum.
inline double ev100_from_average_luminance(double average_luminance_nits) {
    const double nits = std::max(ev100_min_luminance_nits, average_luminance_nits);
    return std::log2(nits * ev100_iso / ev100_meter_calibration);
}

// What to multiply linear RGB in nits by before tone mapping: H = 1 / L_max, where
// L_max = 1.2 * 2^EV100 is the luminance that just saturates the sensor. At EV100 8 a surface of
// 307 nits maps to 1.
inline double exposure_scale(double ev100) {
    return 1.0 / (ev100_saturation_factor * std::exp2(ev100));
}

// Khronos PBR Neutral tone mapping (github.com/KhronosGroup/ToneMapping, version of 2024):
// exposed linear RGB (any non-negative values) to display-referred linear RGB in [0, 1]. Below a
// peak channel of 0.76 it only lifts or lowers the colour by a small offset; above, it compresses
// the peak towards 1 and desaturates towards white. The GPU tiers run the same function in Slang.
template <typename T> glm::vec<3, T> pbr_neutral(glm::vec<3, T> rgb) {
    constexpr T start_compression = T(0.8) - T(0.04);
    constexpr T desaturation = T(0.15);

    const T x = std::min(rgb.r, std::min(rgb.g, rgb.b));
    const T offset = x < T(0.08) ? x - T(6.25) * x * x : T(0.04);
    rgb -= offset;

    const T peak = std::max(rgb.r, std::max(rgb.g, rgb.b));
    if (peak < start_compression) {
        return rgb;
    }

    constexpr T d = T(1) - start_compression;
    const T new_peak = T(1) - d * d / (peak + d - start_compression);
    rgb *= new_peak / peak;

    const T g = T(1) - T(1) / (desaturation * (peak - new_peak) + T(1));
    return rgb * (T(1) - g) + glm::vec<3, T>(new_peak) * g;
}

} // namespace cs
