// SPDX-License-Identifier: Apache-2.0

#include <cs/core/colour.hpp>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/common.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <type_traits>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// --- The Planckian locus, computed here from first principles --------------------------------

// The CIE 1931 2-degree standard observer at 10 nm from 380 to 780 nm: wavelength in nm, then
// x-bar, y-bar, z-bar.
struct Cmf {
    double wavelength_nm;
    double x_bar;
    double y_bar;
    double z_bar;
};

constexpr std::array<Cmf, 41> cie1931_2deg = {{
    {380, 0.001368, 0.000039, 0.006450}, {390, 0.004243, 0.000120, 0.020050},
    {400, 0.014310, 0.000396, 0.067850}, {410, 0.043510, 0.001210, 0.207400},
    {420, 0.134380, 0.004000, 0.645600}, {430, 0.283900, 0.011600, 1.385600},
    {440, 0.348280, 0.023000, 1.747060}, {450, 0.336200, 0.038000, 1.772110},
    {460, 0.290800, 0.060000, 1.669200}, {470, 0.195360, 0.090980, 1.287640},
    {480, 0.095640, 0.139020, 0.812950}, {490, 0.032010, 0.208020, 0.465180},
    {500, 0.004900, 0.323000, 0.272000}, {510, 0.009300, 0.503000, 0.158200},
    {520, 0.063270, 0.710000, 0.078250}, {530, 0.165500, 0.862000, 0.042160},
    {540, 0.290400, 0.954000, 0.020300}, {550, 0.433450, 0.994950, 0.008750},
    {560, 0.594500, 0.995000, 0.003900}, {570, 0.762100, 0.952000, 0.002100},
    {580, 0.916300, 0.870000, 0.001650}, {590, 1.026300, 0.757000, 0.001100},
    {600, 1.062200, 0.631000, 0.000800}, {610, 1.002600, 0.503000, 0.000340},
    {620, 0.854450, 0.381000, 0.000190}, {630, 0.642400, 0.265000, 0.000050},
    {640, 0.447900, 0.175000, 0.000020}, {650, 0.283500, 0.107000, 0.0},
    {660, 0.164900, 0.061000, 0.0},      {670, 0.087400, 0.032000, 0.0},
    {680, 0.046770, 0.017000, 0.0},      {690, 0.022700, 0.008210, 0.0},
    {700, 0.011359, 0.004102, 0.0},      {710, 0.005790, 0.002091, 0.0},
    {720, 0.002899, 0.001047, 0.0},      {730, 0.001440, 0.000520, 0.0},
    {740, 0.000690, 0.000249, 0.0},      {750, 0.000332, 0.000120, 0.0},
    {760, 0.000166, 0.000060, 0.0},      {770, 0.000083, 0.000030, 0.0},
    {780, 0.000042, 0.000015, 0.0},
}};

// The second radiation constant c2 = h c / k_B, in metre kelvin.
constexpr double c2_m_k = 1.4388e-2;

// CIE XYZ of a blackbody at kelvin, up to a constant factor: Planck's law
// M(lambda) ~ lambda^-5 / (exp(c2 / (lambda T)) - 1) summed against the observer at 10 nm steps.
// The first radiation constant and the step width scale X, Y and Z alike and drop out of the
// chromaticity, so they are left out.
glm::dvec3 blackbody_xyz(double kelvin) {
    glm::dvec3 xyz(0.0);
    for (const Cmf& row : cie1931_2deg) {
        const double lambda_m = row.wavelength_nm * 1e-9;
        const double m = std::pow(lambda_m, -5.0) / std::expm1(c2_m_k / (lambda_m * kelvin));
        xyz += m * glm::dvec3(row.x_bar, row.y_bar, row.z_bar);
    }
    return xyz;
}

// CIE 1931 chromaticity (x, y).
glm::dvec2 xy_from_xyz(const glm::dvec3& xyz) {
    const double sum = xyz.x + xyz.y + xyz.z;
    return {xyz.x / sum, xyz.y / sum};
}

// CIE 1960 UCS chromaticity: u = 4X / (X + 15Y + 3Z), v = 6Y / (X + 15Y + 3Z).
glm::dvec2 uv_from_xyz(const glm::dvec3& xyz) {
    const double d = xyz.x + 15.0 * xyz.y + 3.0 * xyz.z;
    return {4.0 * xyz.x / d, 6.0 * xyz.y / d};
}

// Linear Rec. 709 (sRGB primaries, D65 white) to CIE XYZ, the matrix derived from the primaries
// (Lindbloom).
glm::dvec3 xyz_from_linear_rec709(const glm::dvec3& rgb) {
    return {
        0.4124564 * rgb.r + 0.3575761 * rgb.g + 0.1804375 * rgb.b,
        0.2126729 * rgb.r + 0.7151522 * rgb.g + 0.0721750 * rgb.b,
        0.0193339 * rgb.r + 0.1191920 * rgb.g + 0.9503041 * rgb.b,
    };
}

double delta_uv(const glm::dvec2& a, const glm::dvec2& b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

bool all_finite(const glm::dvec3& c) {
    return std::isfinite(c.r) && std::isfinite(c.g) && std::isfinite(c.b);
}

double max_abs_difference(const glm::dvec3& a, const glm::dvec3& b) {
    const glm::dvec3 d = glm::abs(a - b);
    return std::max(d.r, std::max(d.g, d.b));
}

// cct_to_linear_rgb normalises with cs::luminance; dividing by a sum of three products and
// multiplying back costs a few double rounding errors (~1e-16 each), so 1e-12 leaves a wide
// margin while still catching normalisation with any other weights (the XYZ matrix's 0.2126729
// row would be off by ~1e-5).
constexpr double unit_luminance_tolerance = 1e-12;

} // namespace

// --- Criterion 1: luminance uses the Rec. 709 weights ------------------------------------------

TEMPLATE_TEST_CASE("luminance uses the Rec. 709 weights (0.2126, 0.7152, 0.0722)",
                   "[core][colour][accuracy]", float, double) {
    using T = TestType;
    using V = glm::vec<3, T>;
    // One rounding of the product and two of the sum: a few ulps of 1. For float that is
    // ~2e-7, for double ~4e-16; the tolerances below are a few times that.
    const T tolerance = std::is_same_v<T, float> ? T(1e-6) : T(1e-14);

    CHECK_THAT(cs::luminance(V(1, 0, 0)), WithinAbs(0.2126, tolerance));
    CHECK_THAT(cs::luminance(V(0, 1, 0)), WithinAbs(0.7152, tolerance));
    CHECK_THAT(cs::luminance(V(0, 0, 1)), WithinAbs(0.0722, tolerance));

    // The weights add up to 1: equal-energy white of 1 nit has luminance 1.
    CHECK_THAT(cs::luminance(V(1, 1, 1)), WithinAbs(1.0, tolerance));
    CHECK(cs::luminance(V(0, 0, 0)) == T(0));

    // Linear in the colour: 0.2126 * 3 + 0.7152 * 5 + 0.0722 * 7 = 0.6378 + 3.576 + 0.5054
    // = 4.7192; and a scale of 100 nits scales the result.
    CHECK_THAT(cs::luminance(V(3, 5, 7)), WithinAbs(4.7192, 8 * tolerance));
    CHECK_THAT(cs::luminance(V(100, 100, 100)), WithinAbs(100.0, 100 * tolerance));
}

// luminance is constexpr, so it must also run at compile time.
static_assert(cs::luminance(glm::dvec3(0.0, 1.0, 0.0)) == 0.7152);

// --- Criterion 2: cct_to_linear_rgb follows the Planckian locus --------------------------------

TEST_CASE("the test's own Planckian locus reproduces CIE Illuminant A",
          "[core][colour][accuracy]") {
    // CIE Illuminant A is a blackbody at 2856 K (with c2 = 1.4388e-2 m K); its published
    // chromaticity, from the 5 nm (or 1 nm) observer, is (0.44757, 0.40745). The 10 nm table here
    // gives (0.447567, 0.407526): summing at a coarser step moves y by ~8e-5. 2e-4 in x and y
    // allows that step error and is still five times tighter than the delta-uv 0.001 the locus is
    // used to check against below (2e-4 in each of x and y is at most 2.2e-4 in uv here).
    const glm::dvec2 xy = xy_from_xyz(blackbody_xyz(2856.0));
    CHECK_THAT(xy.x, WithinAbs(0.44757, 2e-4));
    CHECK_THAT(xy.y, WithinAbs(0.40745, 2e-4));
}

TEST_CASE("cct_to_linear_rgb lies within delta-uv 0.001 of the Planckian locus",
          "[core][colour][accuracy]") {
    // The tolerance is the acceptance criterion's. The header's cubic fit (Kim et al.) is within
    // 0.0004 of the locus; the locus computed here from the 10 nm table is within ~2e-4 of the
    // 1 nm one (see the Illuminant A test); and the RGB to XYZ matrix is the inverse of the one
    // the implementation should use to within its 7 printed digits. Together well under 0.001.
    for (const double kelvin : {2700.0, 3000.0, 4000.0, 6500.0}) {
        INFO(kelvin << " K");
        const glm::dvec3 rgb = cs::cct_to_linear_rgb(kelvin);
        REQUIRE(all_finite(rgb));
        const glm::dvec2 uv = uv_from_xyz(xyz_from_linear_rec709(rgb));
        const glm::dvec2 locus_uv = uv_from_xyz(blackbody_xyz(kelvin));
        INFO("uv " << uv.x << ", " << uv.y << "; locus " << locus_uv.x << ", " << locus_uv.y);
        CHECK(delta_uv(uv, locus_uv) <= 0.001);
        CHECK_THAT(cs::luminance(rgb), WithinAbs(1.0, unit_luminance_tolerance));
    }
}

TEST_CASE("cct_to_linear_rgb at 3000 K is warm: r > g > b", "[core][colour][accuracy]") {
    // A 3000 K blackbody (warm white LED, halogen) has its chromaticity at about xy
    // (0.437, 0.404), far on the red side of D65; in Rec. 709 that is roughly (1.77, 0.85, 0.27).
    const glm::dvec3 rgb = cs::cct_to_linear_rgb(3000.0);
    INFO("rgb " << rgb.r << ", " << rgb.g << ", " << rgb.b);
    CHECK(rgb.r > rgb.g);
    CHECK(rgb.g > rgb.b);
    CHECK(rgb.b > 0.0); // 3000 K is inside the Rec. 709 gamut, so nothing is clipped
}

TEST_CASE("cct_to_linear_rgb at 6500 K is not white", "[core][colour][accuracy]") {
    // A blackbody at 6500 K (or 6504 K) is not D65: the locus passes at xy about (0.3135, 0.3236),
    // while D65 is (0.3127, 0.3290), about delta-uv 0.0032 above the locus on the green side. So
    // the blackbody is slightly magenta (pink) of white, with green the smallest channel; in
    // Rec. 709 it is roughly (1.043, 0.984, 1.035), off white by about 0.04. An implementation
    // that returns (1, 1, 1) there, or uses a daylight formula, fails.
    const glm::dvec3 rgb = cs::cct_to_linear_rgb(6500.0);
    INFO("rgb " << rgb.r << ", " << rgb.g << ", " << rgb.b);

    // 0.01 is a quarter of the expected 0.04 and far above any rounding.
    CHECK(max_abs_difference(rgb, glm::dvec3(1.0)) > 0.01);

    // And its chromaticity is measurably away from D65: about 0.0032, so require 0.002.
    const glm::dvec2 d65_xy(0.3127, 0.3290);
    const glm::dvec3 d65_xyz(d65_xy.x / d65_xy.y, 1.0, (1.0 - d65_xy.x - d65_xy.y) / d65_xy.y);
    CHECK(delta_uv(uv_from_xyz(xyz_from_linear_rec709(rgb)), uv_from_xyz(d65_xyz)) > 0.002);

    // Green is the smallest channel: the blackbody is below D65 in v.
    CHECK(rgb.g < rgb.r);
    CHECK(rgb.g < rgb.b);
}

TEST_CASE("cct_to_linear_rgb is continuous, non-negative and of luminance 1 from 1667 K to "
          "25000 K",
          "[core][colour][accuracy]") {
    // The criterion: no channel steps by more than 0.01 between 1 K neighbours. Over the range
    // the colour moves from about (2.96, 0.52, 0) to (0.73, 0.99, 1.93), steepest at the low end
    // where a 1 K step changes red by ~1.5e-3, so 0.01 catches a jump (a seam between the fit's
    // pieces at 2222 K and 4000 K, or at the blue clip) but not the smooth slope.
    const auto first = static_cast<int>(cs::cct_min_kelvin);
    const auto last = static_cast<int>(cs::cct_max_kelvin);
    REQUIRE(first == 1667);
    REQUIRE(last == 25000);

    glm::dvec3 previous = cs::cct_to_linear_rgb(first);
    double max_step = 0.0;
    int max_step_at = first;
    double max_luminance_error = std::abs(cs::luminance(previous) - 1.0);
    double min_channel = std::min(previous.r, std::min(previous.g, previous.b));
    bool finite = all_finite(previous);
    for (int kelvin = first + 1; kelvin <= last; ++kelvin) {
        const glm::dvec3 rgb = cs::cct_to_linear_rgb(kelvin);
        finite = finite && all_finite(rgb);
        min_channel = std::min(min_channel, std::min(rgb.r, std::min(rgb.g, rgb.b)));
        max_luminance_error = std::max(max_luminance_error, std::abs(cs::luminance(rgb) - 1.0));
        const double step = max_abs_difference(rgb, previous);
        if (step > max_step) {
            max_step = step;
            max_step_at = kelvin;
        }
        previous = rgb;
    }
    INFO("largest step " << max_step << " between " << max_step_at - 1 << " and " << max_step_at
                         << " K");
    CHECK(finite);
    CHECK(max_step <= 0.01);
    // Light is never negative: the blue that falls outside the gamut below ~1900 K is clipped.
    CHECK(min_channel >= 0.0);
    CHECK(max_luminance_error <= unit_luminance_tolerance);
}

TEST_CASE("cct_to_linear_rgb clamps kelvin to [1667, 25000] and maps NaN to 1667 K",
          "[core][colour]") {
    const glm::dvec3 at_min = cs::cct_to_linear_rgb(cs::cct_min_kelvin);
    const glm::dvec3 at_max = cs::cct_to_linear_rgb(cs::cct_max_kelvin);
    REQUIRE(all_finite(at_min));
    REQUIRE(all_finite(at_max));

    // The ends are different colours: a deep orange and a pale blue.
    CHECK(at_min.r > at_min.b);
    CHECK(at_max.b > at_max.r);

    // Clamping passes the end value itself on, so the colours are identical, not just close.
    constexpr double inf = std::numeric_limits<double>::infinity();
    for (const double kelvin : {1666.0, 1000.0, 1.0, 0.0, -0.0, -273.15, -1e9, -inf}) {
        INFO(kelvin << " K");
        CHECK(cs::cct_to_linear_rgb(kelvin) == at_min);
    }
    for (const double kelvin : {25001.0, 30000.0, 1e9, std::numeric_limits<double>::max(), inf}) {
        INFO(kelvin << " K");
        CHECK(cs::cct_to_linear_rgb(kelvin) == at_max);
    }

    // NaN is not a temperature; the header gives it the low end rather than a NaN colour.
    const glm::dvec3 from_nan = cs::cct_to_linear_rgb(std::numeric_limits<double>::quiet_NaN());
    CHECK(all_finite(from_nan));
    CHECK(from_nan == at_min);
}

// --- Criterion 3: physical camera exposure -----------------------------------------------------

TEST_CASE("EV100 of a black frame is finite", "[core][colour]") {
    // A room with the lights off averages 0 nits; the meter reads its floor instead of -inf, so
    // the exposure scale stays finite. NaN meters as the floor too.
    const double floor_ev = std::log2(cs::ev100_min_luminance_nits * 8.0);
    for (const double nits : {0.0, -1.0, 1e-9, std::numeric_limits<double>::quiet_NaN()}) {
        INFO("nits " << nits);
        CHECK_THAT(cs::ev100_from_average_luminance(nits), WithinAbs(floor_ev, 1e-12));
        CHECK(std::isfinite(cs::exposure_scale(cs::ev100_from_average_luminance(nits))));
    }
    // One infinite sample makes the average infinite; the meter reads its ceiling, so the scale
    // stays positive instead of turning the frame black.
    const double inf = std::numeric_limits<double>::infinity();
    CHECK_THAT(cs::ev100_from_average_luminance(inf),
               WithinAbs(std::log2(cs::ev100_max_luminance_nits * 8.0), 1e-12));
    CHECK(cs::exposure_scale(cs::ev100_from_average_luminance(inf)) > 0.0);
}

TEST_CASE("EV100 and exposure scale at 1 nit and 1000 nits", "[core][colour][accuracy]") {
    // EV100 = log2(L * 100 / 12.5) = log2(8 L); H = 1 / (1.2 * 2^EV100).
    // The functions are one log2 or exp2 and a product, so they are good to a few double ulps;
    // 1e-12 (relative for H) leaves a wide margin.
    //
    // 1 nit: EV100 = log2(8) = 3; H = 1 / (1.2 * 8) = 1 / 9.6 = 0.104166...
    CHECK_THAT(cs::ev100_from_average_luminance(1.0), WithinAbs(3.0, 1e-12));
    CHECK_THAT(cs::exposure_scale(3.0), WithinRel(1.0 / 9.6, 1e-12));

    // 1000 nits: EV100 = log2(8000) = 3 + 3 log2(10) = 3 + 3 * 3.321928094887362
    // = 12.965784284662087; H = 1 / (1.2 * 8000) = 1 / 9600.
    CHECK_THAT(cs::ev100_from_average_luminance(1000.0), WithinAbs(12.965784284662087, 1e-12));
    CHECK_THAT(cs::exposure_scale(12.965784284662087), WithinRel(1.0 / 9600.0, 1e-12));

    // The header's example: at EV100 8, L_max = 1.2 * 256 = 307.2 nits maps to 1.
    CHECK_THAT(307.2 * cs::exposure_scale(8.0), WithinRel(1.0, 1e-12));

    // Metering then exposing puts the average luminance at L * H = L / (1.2 * 8 L) = 1 / 9.6,
    // whatever L is: the meter's calibration point, about 0.8 stop below 18 % grey.
    for (const double nits : {1e-3, 0.5, 1.0, 50.0, 1000.0, 1e6}) {
        INFO(nits << " nits");
        const double ev100 = cs::ev100_from_average_luminance(nits);
        CHECK_THAT(nits * cs::exposure_scale(ev100), WithinRel(1.0 / 9.6, 1e-12));
    }

    // One stop more light is one EV more and half the scale.
    CHECK_THAT(cs::ev100_from_average_luminance(2.0) - cs::ev100_from_average_luminance(1.0),
               WithinAbs(1.0, 1e-12));
    CHECK_THAT(cs::exposure_scale(4.0) / cs::exposure_scale(3.0), WithinRel(0.5, 1e-12));
}

// --- Criterion 4: Khronos PBR Neutral ----------------------------------------------------------

namespace {

struct ToneMapCase {
    glm::dvec3 input;
    glm::dvec3 expected;
    const char* what;
};

// Expected outputs from a direct Python transcription of the Khronos GLSL reference
// (github.com/KhronosGroup/ToneMapping, PBR_Neutral/pbrNeutral.glsl, 2024), run in double
// precision outside this repository and printed to 12 significant digits:
//   startCompression = 0.8 - 0.04; desaturation = 0.15
//   x = min(r, g, b); offset = x < 0.08 ? x - 6.25 x^2 : 0.04; color -= offset
//   peak = max(color); if peak < startCompression: return color
//   d = 1 - startCompression; newPeak = 1 - d^2 / (peak + d - startCompression)
//   color *= newPeak / peak; g = 1 - 1 / (desaturation (peak - newPeak) + 1)
//   return mix(color, newPeak, g)
const std::array<ToneMapCase, 10> pbr_neutral_cases = {{
    {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, "black"},
    // x = 0.02 < 0.08: offset = 0.02 - 6.25 * 0.0004 = 0.0175.
    {{0.02, 0.03, 0.05}, {0.0025, 0.0125, 0.0325}, "dark, offset below 0.08"},
    // x = 0.1 >= 0.08: offset = 0.04.
    {{0.1, 0.2, 0.3}, {0.06, 0.16, 0.26}, "dark, offset 0.04"},
    // Peak after the offset is 0.71 < 0.76: only the offset.
    {{0.75, 0.6, 0.4}, {0.71, 0.56, 0.36}, "below the compression knee"},
    // Peak 0.78, just above 0.76: compressed to 0.778461538462.
    {{0.82, 0.82, 0.82}, {0.778461538462, 0.778461538462, 0.778461538462}, "just above the knee"},
    {{1.0, 0.0, 0.0}, {0.88, 0.0155599214145, 0.0155599214145}, "saturated red"},
    {{0.0, 0.9, 0.2}, {0.00651370422191, 0.848421052632, 0.193604226091}, "saturated green"},
    {{10.0, 10.0, 10.0}, {0.993898305085, 0.993898305085, 0.993898305085}, "bright grey"},
    {{100.0, 50.0, 20.0}, {0.999420756235, 0.967868900591, 0.948937787205}, "very bright, warm"},
    {{1.2, 0.7, 0.3}, {0.91, 0.531936019942, 0.229484835895}, "warm highlight"},
}};

} // namespace

TEMPLATE_TEST_CASE("pbr_neutral matches the Khronos reference at 10 inputs within 1e-5",
                   "[core][colour][accuracy]", float, double) {
    using T = TestType;
    using V = glm::vec<3, T>;
    // 1e-5 is the acceptance criterion's tolerance. The expected values carry 12 digits, so they
    // are exact to ~1e-12; float arithmetic on outputs in [0, 1] costs a few times 6e-8.
    for (const ToneMapCase& c : pbr_neutral_cases) {
        INFO(c.what << ": (" << c.input.r << ", " << c.input.g << ", " << c.input.b << ")");
        const V out = cs::pbr_neutral(V(c.input));
        CHECK_THAT(out.r, WithinAbs(c.expected.r, 1e-5));
        CHECK_THAT(out.g, WithinAbs(c.expected.g, 1e-5));
        CHECK_THAT(out.b, WithinAbs(c.expected.b, 1e-5));
    }
}

TEMPLATE_TEST_CASE("pbr_neutral maps non-negative input into [0, 1] and keeps grey grey",
                   "[core][colour]", float, double) {
    using T = TestType;
    using V = glm::vec<3, T>;
    for (const T level :
         {T(0), T(0.001), T(0.08), T(0.5), T(0.76), T(0.8), T(1), T(4), T(1e3), T(1e6)}) {
        INFO("grey " << level);
        const V out = cs::pbr_neutral(V(level));
        CHECK(out.r == out.g);
        CHECK(out.g == out.b);
        CHECK(out.r >= T(0));
        // Above the knee the result is mix(newPeak, newPeak, g) with newPeak just under 1; the
        // mix can round one ulp above it, which is not a meaningful overshoot.
        CHECK(out.r <= T(1) + std::numeric_limits<T>::epsilon());
    }
}

TEMPLATE_TEST_CASE("pbr_neutral takes negative and NaN channels as 0", "[core][colour]", float,
                   double) {
    using T = TestType;
    using V = glm::vec<3, T>;
    // The reference is undefined there; a negative minimum would make its offset negative and
    // brighten the whole colour. Exact equality: both sides run the same arithmetic.
    const T nan = std::numeric_limits<T>::quiet_NaN();
    CHECK(cs::pbr_neutral(V(-0.5, 0.2, 0.2)) == cs::pbr_neutral(V(0, 0.2, 0.2)));
    CHECK(cs::pbr_neutral(V(nan, 0.5, 2.0)) == cs::pbr_neutral(V(0, 0.5, 2.0)));
    CHECK(cs::pbr_neutral(V(-1, -1, -1)) == V(0));
}
