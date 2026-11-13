// SPDX-License-Identifier: Apache-2.0

#include <cs/core/colour.hpp>

#include <glm/common.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>
#include <glm/vec2.hpp>

namespace cs {

namespace {

// The Planckian locus in CIE 1931 xy, as cubic polynomials: x in 1/T, then y in x, each in
// pieces. Kim et al., US patent 7,024,034 (2006). Within delta-uv 0.0004 of the locus integrated
// from Planck's law and the CIE 1931 2-degree observer, and steps of under 0.001 in RGB where
// the pieces meet at 2222 K and 4000 K.
glm::dvec2 planckian_xy(double kelvin) {
    const double t = 1.0 / kelvin;
    const double x = kelvin <= 4000.0
                         ? ((-0.2661239e9 * t - 0.2343589e6) * t + 0.8776956e3) * t + 0.179910
                         : ((-3.0258469e9 * t + 2.1070379e6) * t + 0.2226347e3) * t + 0.240390;
    if (kelvin <= 2222.0) {
        return {x, ((-1.1063814 * x - 1.34811020) * x + 2.18555832) * x - 0.20219683};
    }
    if (kelvin <= 4000.0) {
        return {x, ((-0.9549476 * x - 1.37418593) * x + 2.09137015) * x - 0.16748867};
    }
    return {x, ((3.0817580 * x - 5.87338670) * x + 3.75112997) * x - 0.37001483};
}

// CIE XYZ to linear Rec. 709, the sRGB (D65) matrix of IEC 61966-2-1, written row by row; glm is
// column-major, hence the transpose.
const glm::dmat3 xyz_to_linear_rec709 =
    glm::transpose(glm::dmat3{3.2404542, -1.5371385, -0.4985314,  //
                              -0.9692660, 1.8760108, 0.0415560,   //
                              0.0556434, -0.2040259, 1.0572252}); //

} // namespace

glm::dvec3 cct_to_linear_rgb(double kelvin) {
    // NaN fails the first comparison and becomes the lowest temperature.
    kelvin = !(kelvin > cct_min_kelvin) ? cct_min_kelvin : std::min(kelvin, cct_max_kelvin);

    // xyY with Y = 1 to XYZ, then to linear Rec. 709.
    const glm::dvec2 xy = planckian_xy(kelvin);
    const glm::dvec3 xyz{xy.x / xy.y, 1.0, (1.0 - xy.x - xy.y) / xy.y};
    const glm::dvec3 rgb = xyz_to_linear_rec709 * xyz;

    // Clip what lies outside the gamut (over this range only blue, below about 1900 K), then
    // bring the luminance back to exactly 1. Red stays well above zero, so the luminance never
    // vanishes.
    const glm::dvec3 clipped = glm::max(rgb, glm::dvec3(0.0));
    return clipped / luminance(clipped);
}

} // namespace cs
