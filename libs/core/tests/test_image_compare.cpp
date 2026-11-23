// SPDX-License-Identifier: Apache-2.0

// Tests of image_compare.hpp (M1-07): the mean absolute error, the maximum error and the mean
// NVIDIA FLIP error of a test image against a reference; and (M1-18) the FLIP heat map the
// golden-image runner writes when a comparison fails.

#include <cs/core/image.hpp>
#include <cs/core/image_compare.hpp>
#include <cs/core/image_io.hpp>
#include <cs/core/random.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The reference implementation's magma map, FLIP::MapMagma, for the heat map tests. Last, as it
// defines the macros Max and Min.
#include <FLIP.h>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

cs::Image uniform(int width, int height, cs::Rgb value) {
    cs::Image image(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.at(x, y) = value;
        }
    }
    return image;
}

// Values in [0, scale), seeded by `seed` and the pixel.
cs::Image random_image(int width, int height, std::uint64_t seed, float scale) {
    cs::Image image(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            cs::Rng rng(cs::hash_seed(seed, static_cast<std::uint32_t>(x),
                                      static_cast<std::uint32_t>(y), 0, 0));
            const float r = scale * rng.next_float();
            const float g = scale * rng.next_float();
            const float b = scale * rng.next_float();
            image.at(x, y) = {r, g, b};
        }
    }
    return image;
}

bool valid_flip(double mean_flip) {
    return std::isfinite(mean_flip) && mean_flip >= 0.0 && mean_flip <= 1.0;
}

// The message compare throws for images of different sizes; fails the test if it throws nothing.
// The message is returned from after the handler: FAIL throws, so a return after it is unreachable
// (MSVC's warning C4702 in a debug build), and FAIL's own exception is no std::invalid_argument.
std::string size_error_message(const cs::Image& reference, const cs::Image& test) {
    std::string message;
    try {
        (void)cs::compare(reference, test);
        FAIL("compare did not throw std::invalid_argument for images of different sizes");
    } catch (const std::invalid_argument& error) {
        message = error.what();
    }
    return message;
}

// 0.25 and 0.75 are exact in float and their difference, 0.5, is exact too; so is 0.5 / 4096 =
// 2^-13 in double. The expected errors below are therefore exact, whatever order the sum is in:
// the other differences are all exactly 0.
constexpr float base = 0.25f;
constexpr float changed = 0.75f;
constexpr int side = 64;
constexpr int pixel_count = side * side; // 4096

} // namespace

// --- Identical images ----------------------------------------------------------------------------

TEST_CASE("AC3: identical images give 0 for all three errors", "[core][image_compare]") {
    const cs::Image reference = random_image(side, side, 60, 10.0f);
    const cs::Image copy = reference;

    for (const cs::ImageComparison& result :
         {cs::compare(reference, copy), cs::compare(reference, reference)}) {
        CHECK(result.mean_absolute_error == 0.0);
        CHECK(result.max_error == 0.0);
        CHECK(result.mean_flip == 0.0);
    }
}

TEST_CASE("AC3: two black images give 0 for all three errors, not NaN", "[core][image_compare]") {
    // A black reference has no exposure to find FLIP's exposures from (image_compare.hpp says they
    // are found from the test image then); a black test image as well must still give 0.
    const cs::Image black(side, side);
    const cs::ImageComparison result = cs::compare(black, black);
    CHECK(result.mean_absolute_error == 0.0);
    CHECK(result.max_error == 0.0);
    CHECK(result.mean_flip == 0.0);
}

TEST_CASE("AC3: identical 1x1 images give 0 for all three errors", "[core][image_compare]") {
    const cs::Image pixel = uniform(1, 1, {0.2f, 0.4f, 0.8f});
    const cs::ImageComparison result = cs::compare(pixel, pixel);
    CHECK(result.mean_absolute_error == 0.0);
    CHECK(result.max_error == 0.0);
    CHECK(result.mean_flip == 0.0);
}

// --- One changed pixel ---------------------------------------------------------------------------

TEST_CASE("AC3: one pixel of 64x64 changed by 0.5 in all channels gives a mean absolute error of "
          "exactly 0.5/4096",
          "[core][image_compare]") {
    // Mean over 4096 pixels and 3 channels: 3 * 0.5 / (3 * 4096) = 0.5 / 4096, which is the
    // criterion's "0.5/4096 per channel" too, as every channel's own mean is 0.5 / 4096.
    const cs::Image reference = uniform(side, side, {base, base, base});
    cs::Image test = reference;
    test.at(17, 42) = {changed, changed, changed};

    const cs::ImageComparison result = cs::compare(reference, test);
    CHECK(result.mean_absolute_error == 0.5 / pixel_count);
    CHECK(result.max_error == 0.5);
    CHECK(result.mean_flip > 0.0);
    CHECK(valid_flip(result.mean_flip));
}

TEST_CASE("AC3: the mean absolute error is the same whichever image is brighter",
          "[core][image_compare]") {
    // |reference - test|: the pixel 0.5 darker in the test image counts as much as 0.5 brighter.
    const cs::Image reference = uniform(side, side, {changed, changed, changed});
    cs::Image test = reference;
    test.at(0, 63) = {base, base, base};

    const cs::ImageComparison result = cs::compare(reference, test);
    CHECK(result.mean_absolute_error == 0.5 / pixel_count);
    CHECK(result.max_error == 0.5);
}

TEST_CASE("AC3: one channel of one pixel changed by 0.5 gives 0.5/(3*4096)",
          "[core][image_compare]") {
    const cs::Image reference = uniform(side, side, {base, base, base});
    cs::Image test = reference;
    test.at(63, 0).g = changed;

    const cs::ImageComparison result = cs::compare(reference, test);
    // 0.5 / 12288 = 2^-13 / 3 is not exact in double. Dividing the exact sum 0.5 by 12288, or the
    // exact per-channel mean 2^-13 by 3, rounds once to the same double; 1e-12 relative allows a
    // different but sound order of operations.
    CHECK_THAT(result.mean_absolute_error, WithinRel(0.5 / (3.0 * pixel_count), 1e-12));
    CHECK(result.max_error == 0.5);
    CHECK(result.mean_flip > 0.0);
    CHECK(valid_flip(result.mean_flip));
}

TEST_CASE("AC3: changing every pixel gives a larger mean FLIP than changing one",
          "[core][image_compare]") {
    const cs::Image reference = uniform(side, side, {base, base, base});
    cs::Image one = reference;
    one.at(31, 31) = {changed, changed, changed};
    const cs::Image all = uniform(side, side, {changed, changed, changed});

    const cs::ImageComparison one_result = cs::compare(reference, one);
    const cs::ImageComparison all_result = cs::compare(reference, all);
    CHECK(all_result.mean_absolute_error == 0.5);
    CHECK(all_result.max_error == 0.5);
    CHECK(valid_flip(all_result.mean_flip));
    CHECK(all_result.mean_flip > one_result.mean_flip);
}

TEST_CASE("AC3: compare of 1x1 images", "[core][image_compare]") {
    // The smallest image, smaller than FLIP's filters.
    const cs::Image reference = uniform(1, 1, {base, base, base});
    const cs::Image test = uniform(1, 1, {changed, base, base});
    const cs::ImageComparison result = cs::compare(reference, test);
    CHECK_THAT(result.mean_absolute_error, WithinRel(0.5 / 3.0, 1e-12));
    CHECK(result.max_error == 0.5);
    CHECK(valid_flip(result.mean_flip));
}

TEST_CASE("AC3: the numeric errors of random images match a direct calculation",
          "[core][image_compare]") {
    // Non-square, to catch a transposed width and height.
    const cs::Image reference = random_image(48, 40, 61, 10.0f);
    const cs::Image test = random_image(48, 40, 62, 10.0f);

    double sum = 0.0;
    double max = 0.0;
    for (int y = 0; y < 40; ++y) {
        for (int x = 0; x < 48; ++x) {
            const cs::Rgb& r = reference.at(x, y);
            const cs::Rgb& t = test.at(x, y);
            for (const double d : {std::abs(static_cast<double>(r.r) - t.r),
                                   std::abs(static_cast<double>(r.g) - t.g),
                                   std::abs(static_cast<double>(r.b) - t.b)}) {
                sum += d;
                max = std::max(max, d);
            }
        }
    }
    const double mean = sum / (48.0 * 40.0 * 3.0);

    const cs::ImageComparison result = cs::compare(reference, test);
    // Here the differences are taken in double. Taken in float they round by up to 2^-24 relative
    // each, so the mean and the maximum agree within 1e-6 relative either way.
    CHECK_THAT(result.mean_absolute_error, WithinRel(mean, 1e-6));
    CHECK_THAT(result.max_error, WithinRel(max, 1e-6));
    CHECK(result.mean_flip > 0.0);
    CHECK(valid_flip(result.mean_flip));

    // |a - b| = |b - a|, term by term, so the swapped numeric errors are the same.
    const cs::ImageComparison swapped = cs::compare(test, reference);
    CHECK(swapped.mean_absolute_error == result.mean_absolute_error);
    CHECK(swapped.max_error == result.max_error);
    CHECK(valid_flip(swapped.mean_flip));
}

// --- Different sizes -----------------------------------------------------------------------------

TEST_CASE("AC5: compare refuses images of different sizes, naming both sizes",
          "[core][image_compare]") {
    const cs::Image reference(64, 48);
    const cs::Image test(32, 17);
    CHECK_THROWS_AS(cs::compare(reference, test), std::invalid_argument);

    const std::string message = size_error_message(reference, test);
    INFO("message: " << message);
    CHECK(message.find("64") != std::string::npos);
    CHECK(message.find("48") != std::string::npos);
    CHECK(message.find("32") != std::string::npos);
    CHECK(message.find("17") != std::string::npos);
}

TEST_CASE("AC5: compare refuses images that differ in only one dimension",
          "[core][image_compare]") {
    CHECK_THROWS_AS(cs::compare(cs::Image(64, 64), cs::Image(64, 63)), std::invalid_argument);
    CHECK_THROWS_AS(cs::compare(cs::Image(64, 64), cs::Image(63, 64)), std::invalid_argument);
    // Transposed: the same number of pixels is not the same size.
    CHECK_THROWS_AS(cs::compare(cs::Image(64, 32), cs::Image(32, 64)), std::invalid_argument);
    CHECK_THROWS_AS(cs::compare(cs::Image(32, 64), cs::Image(64, 32)), std::invalid_argument);
}

// --- The committed pair --------------------------------------------------------------------------

TEST_CASE("AC4: mean FLIP of the committed pair matches the reference implementation within 1e-4",
          "[core][image_compare][accuracy]") {
    // How the expected value was produced: by NVIDIA's reference implementation alone, as its
    // FLIP tool treats EXR input. A program using only tinyexr's LoadEXR and FLIP.h's
    // FLIP::evaluate (interleaved floats, HDR-FLIP, default parameters: 67.02 pixels per degree,
    // ACES, exposures -11.147449 to -4.998166 in 7 steps, found from the reference) printed
    // 0.145033047 for this pair. libs/core/tests/data/README.md has the program and the command.
    // It shares no code with cs::read_exr or cs::compare, so this checks our reader and our use
    // of FLIP against the reference rather than against themselves.
    //
    // The tolerance, 1e-4, is the criterion's. The reference prints the mean as a float, and
    // ours sums in double; they differ by about 1e-8.
    constexpr double expected_mean_flip = 0.145033047;

    const std::filesystem::path data =
        std::filesystem::path(CS_SOURCE_DIR) / "libs/core/tests/data";
    const std::filesystem::path reference_path = data / "flip_reference.exr";
    const std::filesystem::path test_path = data / "flip_test.exr";
    REQUIRE(std::filesystem::exists(reference_path));
    REQUIRE(std::filesystem::exists(test_path));

    // At most 20 KB each, read as 20 000 bytes, the stricter of 20 kB and 20 KiB.
    CHECK(std::filesystem::file_size(reference_path) <= 20'000);
    CHECK(std::filesystem::file_size(test_path) <= 20'000);

    const cs::Image reference = cs::read_exr(reference_path);
    const cs::Image test = cs::read_exr(test_path);
    REQUIRE(reference.width() == test.width());
    REQUIRE(reference.height() == test.height());

    const cs::ImageComparison result = cs::compare(reference, test);
    INFO("mean FLIP: " << result.mean_flip);
    // A pair that FLIP cannot tell apart would not test FLIP.
    CHECK(result.mean_flip > 0.0);
    CHECK(valid_flip(result.mean_flip));
    CHECK_THAT(result.mean_flip, WithinAbs(expected_mean_flip, 1e-4));

    // The reference against itself is indistinguishable.
    CHECK(cs::compare(reference, reference).mean_flip == 0.0);
}

// The image the committed pair is made of (libs/core/tests/data/README.md): a wall in nits lit by a
// spotlight pool, a striped painting and a lamp at the top; the test image has seeded noise of
// +-20 % and the painting one pixel to the right.
static cs::Image flip_pair_image(bool test) {
    constexpr int width = 48;
    constexpr int height = 32;
    constexpr cs::Rgb albedo[] = {{0.6f, 0.1f, 0.1f},
                                  {0.1f, 0.4f, 0.6f},
                                  {0.8f, 0.7f, 0.2f},
                                  {0.05f, 0.05f, 0.05f},
                                  {0.3f, 0.6f, 0.2f}};
    cs::Image image(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float dx = (static_cast<float>(x) - 24.0f) / 16.0f;
            const float dy = (static_cast<float>(y) - 14.0f) / 12.0f;
            const float pool = 40.0f + 160.0f * std::exp(-(dx * dx + dy * dy));
            cs::Rgb rgb{pool * 0.95f, pool * 0.92f, pool * 0.85f};
            const int px = test ? x - 1 : x;
            if (px >= 14 && px < 34 && y >= 8 && y < 22) {
                const cs::Rgb& a = albedo[(px - 14) / 4];
                rgb = {pool * a.r, pool * a.g, pool * a.b};
            }
            if (y < 3 && x >= 22 && x < 26) {
                rgb = {5000.0f, 4800.0f, 4300.0f};
            }
            if (test) {
                cs::Rng rng(cs::hash_seed(7, static_cast<std::uint32_t>(x),
                                          static_cast<std::uint32_t>(y), 0, 0));
                const float noise = 0.8f + 0.4f * rng.next_float();
                rgb = {rgb.r * noise, rgb.g * noise, rgb.b * noise};
            }
            image.at(x, y) = rgb;
        }
    }
    return image;
}

// Hidden: run only on purpose, as it overwrites the committed pair. The expected mean FLIP
// above must then be produced again with the reference program.
TEST_CASE("generate the committed FLIP pair", "[core][image_compare][.generate-flip-pair]") {
    const std::filesystem::path data =
        std::filesystem::path(CS_SOURCE_DIR) / "libs/core/tests/data";
    cs::write_exr(flip_pair_image(false), data / "flip_reference.exr", cs::ExrChannels::half);
    cs::write_exr(flip_pair_image(true), data / "flip_test.exr", cs::ExrChannels::half);
}

// --- Values FLIP cannot see ----------------------------------------------------------------------

TEST_CASE("compare with a NaN in the test image gives NaN numeric errors and a finite FLIP",
          "[core][image_compare]") {
    const cs::Image reference = uniform(side, side, {base, base, base});
    cs::Image test = reference;
    test.at(3, 4).g = std::numeric_limits<float>::quiet_NaN();

    const cs::ImageComparison result = cs::compare(reference, test);
    CHECK(std::isnan(result.mean_absolute_error));
    CHECK(std::isnan(result.max_error));
    CHECK(valid_flip(result.mean_flip));
}

TEST_CASE("compare with a NaN in the reference gives NaN numeric errors and a finite FLIP",
          "[core][image_compare]") {
    cs::Image reference = uniform(side, side, {base, base, base});
    const cs::Image test = reference;
    reference.at(60, 2).b = std::numeric_limits<float>::quiet_NaN();

    const cs::ImageComparison result = cs::compare(reference, test);
    CHECK(std::isnan(result.mean_absolute_error));
    CHECK(std::isnan(result.max_error));
    CHECK(valid_flip(result.mean_flip));
}

TEST_CASE("compare with an infinity in the test image gives infinite numeric errors and a finite "
          "FLIP",
          "[core][image_compare]") {
    // The numeric errors see values as they are: |0.25 - inf| = inf, so the mean and the maximum
    // are infinite.
    const cs::Image reference = uniform(side, side, {base, base, base});
    cs::Image test = reference;
    test.at(10, 10).r = std::numeric_limits<float>::infinity();

    const cs::ImageComparison result = cs::compare(reference, test);
    CHECK(std::isinf(result.mean_absolute_error));
    CHECK(std::isinf(result.max_error));
    CHECK(valid_flip(result.mean_flip));
}

TEST_CASE("compare: the numeric errors see a negative value as it is, FLIP sees it as 0",
          "[core][image_compare]") {
    const cs::Image reference = uniform(side, side, {base, base, base});
    cs::Image negative = reference;
    negative.at(20, 30) = {-base, -base, -base};
    cs::Image zero = reference;
    zero.at(20, 30) = {0.0f, 0.0f, 0.0f};

    const cs::ImageComparison with_negative = cs::compare(reference, negative);
    // |0.25 - (-0.25)| = 0.5 in each channel of one pixel: exactly 0.5 / 4096, as above.
    CHECK(with_negative.mean_absolute_error == 0.5 / pixel_count);
    CHECK(with_negative.max_error == 0.5);

    // FLIP is given the same images in both cases, so it gives the same answer; 1e-12 allows for a
    // reduction whose order is not fixed.
    const cs::ImageComparison with_zero = cs::compare(reference, zero);
    CHECK_THAT(with_negative.mean_flip, WithinAbs(with_zero.mean_flip, 1e-12));
    CHECK(with_zero.mean_flip > 0.0);
}

TEST_CASE("compare tells apart images darker than FLIP's median floor", "[core][image_compare]") {
    // FLIP takes the median luminance as at least FLT_EPSILON (about 1.2e-7), which for images
    // this dark puts its stop exposure before its start; compare must still see them differ.
    const cs::Image reference = uniform(side, side, {1e-8f, 1e-8f, 1e-8f});
    cs::Image test(side, side);
    const cs::ImageComparison result = cs::compare(reference, test);
    CHECK(result.mean_flip > 0.0);
    CHECK(valid_flip(result.mean_flip));
    CHECK(cs::compare(reference, reference).mean_flip == 0.0);
}

TEST_CASE("compare with a black reference and a test image that is not gives a positive, finite "
          "FLIP",
          "[core][image_compare]") {
    // FLIP's exposures come from the test image when the reference is black (image_compare.hpp);
    // found from the black reference they would be infinite or NaN.
    const cs::Image black(side, side);
    SECTION("uniform grey test image") {
        const cs::ImageComparison result =
            cs::compare(black, uniform(side, side, {0.5f, 0.5f, 0.5f}));
        CHECK(result.mean_absolute_error == 0.5);
        CHECK(result.max_error == 0.5);
        CHECK(result.mean_flip > 0.0);
        CHECK(valid_flip(result.mean_flip));
    }
    SECTION("random test image") {
        const cs::ImageComparison result = cs::compare(black, random_image(side, side, 63, 100.0f));
        CHECK(result.mean_absolute_error > 0.0);
        CHECK(result.mean_flip > 0.0);
        CHECK(valid_flip(result.mean_flip));
    }
    SECTION("one lit pixel") {
        cs::Image test = black;
        test.at(5, 5) = {1000.0f, 1000.0f, 1000.0f};
        const cs::ImageComparison result = cs::compare(black, test);
        CHECK(result.max_error == 1000.0);
        CHECK(result.mean_flip > 0.0);
        CHECK(valid_flip(result.mean_flip));
    }
}

// --- The FLIP heat map (M1-18) -------------------------------------------------------------------
//
// flip_heat_map colours FLIP's error at every pixel with FLIP's magma map, as display-referred
// linear RGB (image_compare.hpp); the golden-image runner writes it as flip.png when a comparison
// fails (M1-18, acceptance criterion 2). The tests below turn each pixel back into the index of its
// magma entry, with the map taken from the reference implementation's FLIP.h, and check the
// indices against compare(): an error e takes entry round(255 e), so index / 255 is within 1/510
// of e at every pixel, and the mean of index / 255 within 1/510 of compare's mean_flip.

namespace {

// The sRGB decoding curve of IEC 61966-2-1, in double.
double srgb_to_linear(double encoded) {
    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
}

using LinearRgb = std::array<double, 3>;

// FLIP's magma map as linear RGB: its 256 sRGB entries through the sRGB decoding curve.
const std::array<LinearRgb, 256>& magma_linear() {
    static const std::array<LinearRgb, 256> map = [] {
        std::array<LinearRgb, 256> linear{};
        for (std::size_t i = 0; i < linear.size(); ++i) {
            // color3's constructor sets x, y and z, which are its r, g and b.
            const FLIP::color3& srgb = FLIP::MapMagma[i];
            linear[i] = {srgb_to_linear(srgb.x), srgb_to_linear(srgb.y), srgb_to_linear(srgb.z)};
        }
        return linear;
    }();
    return map;
}

double channel_distance(const LinearRgb& a, const LinearRgb& b) {
    return std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])});
}

LinearRgb as_linear(const cs::Rgb& pixel) {
    return {pixel.r, pixel.g, pixel.b};
}

// How far a heat map pixel may be from a magma entry, in the channel that differs most, and still
// count as that entry. Two entries differ by at least 3.4e-4 in some channel (entries 0 and 1 are
// the closest pair; the first heat map test checks it); float arithmetic and either common form of
// the sRGB curve move an entry by less than 1e-6. 1e-5 lies well between the two.
constexpr double magma_tolerance = 1e-5;

// The magma entry nearest a pixel, and how far the pixel is from it. A pixel with a NaN channel is
// near none: index -1 at an infinite distance.
struct MagmaMatch {
    int index = -1;
    double distance = std::numeric_limits<double>::infinity();
};

MagmaMatch nearest_magma(const cs::Rgb& pixel) {
    MagmaMatch best;
    const std::array<LinearRgb, 256>& map = magma_linear();
    for (std::size_t i = 0; i < map.size(); ++i) {
        const double distance = channel_distance(as_linear(pixel), map[i]);
        if (distance < best.distance) {
            best = {static_cast<int>(i), distance};
        }
    }
    return best;
}

bool in_unit_range(float channel) {
    return std::isfinite(channel) && channel >= 0.0f && channel <= 1.0f;
}

// A heat map read back into magma indices.
struct HeatMapIndices {
    // Per pixel, row by row; -1 where the pixel is not a magma colour.
    std::vector<int> index;
    // The mean of index / 255 over all pixels, a pixel that is not a magma colour counting as 0.
    double mean = 0.0;
    // Pixels with a channel outside [0, 1], NaN or infinite.
    int out_of_range = 0;
    // Pixels farther than magma_tolerance from every entry.
    int not_magma = 0;
    std::string first_problem;
};

HeatMapIndices read_heat_map(const cs::Image& heat) {
    HeatMapIndices result;
    double sum = 0.0;
    for (int y = 0; y < heat.height(); ++y) {
        for (int x = 0; x < heat.width(); ++x) {
            const cs::Rgb& pixel = heat.at(x, y);
            const MagmaMatch match = nearest_magma(pixel);
            const bool in_range =
                in_unit_range(pixel.r) && in_unit_range(pixel.g) && in_unit_range(pixel.b);
            const bool magma = match.distance <= magma_tolerance;
            result.out_of_range += in_range ? 0 : 1;
            result.not_magma += magma ? 0 : 1;
            if ((!in_range || !magma) && result.first_problem.empty()) {
                std::ostringstream out;
                out.precision(9);
                out << "heat map pixel (" << x << ", " << y << ") is (" << pixel.r << ", "
                    << pixel.g << ", " << pixel.b << "), " << match.distance
                    << " from the nearest magma entry, " << match.index;
                result.first_problem = out.str();
            }
            result.index.push_back(magma ? match.index : -1);
            if (magma) {
                sum += match.index / 255.0;
            }
        }
    }
    result.mean = sum / (static_cast<double>(heat.width()) * heat.height());
    return result;
}

// Checks the heat map of a pair against image_compare.hpp: the size of the inputs, every channel
// in [0, 1], every pixel one of magma's colours, and the mean of index / 255 within 1/510 of
// compare's mean_flip. Entry round(255 e) is within 1/510 of e at every pixel, so the means are
// too; 1e-6 more allows FLIP's float rounding of 255 e + 0.5 and a different order of summation.
void check_heat_map_is_flip_error(const cs::Image& reference, const cs::Image& test) {
    const cs::Image heat = cs::flip_heat_map(reference, test);
    REQUIRE(heat.width() == reference.width());
    REQUIRE(heat.height() == reference.height());
    const HeatMapIndices indices = read_heat_map(heat);
    INFO(indices.first_problem);
    CHECK(indices.out_of_range == 0);
    CHECK(indices.not_magma == 0);

    const double mean_flip = cs::compare(reference, test).mean_flip;
    INFO("compare's mean FLIP: " << mean_flip
                                 << ", the heat map's mean index / 255: " << indices.mean);
    CHECK_THAT(indices.mean, WithinAbs(mean_flip, 1.0 / 510.0 + 1e-6));
}

// Magma's first entry, sRGB (0.001462, 0.000466, 0.013866), as linear RGB. All three lie below
// sRGB 0.04045, where the decoding curve is linear, c / 12.92.
constexpr LinearRgb magma_first = {0.001462 / 12.92, 0.000466 / 12.92, 0.013866 / 12.92};

// The pixels of a heat map that are not magma's first entry, and the first of them. 1e-5 relative
// allows the map's float entries (relative error 3e-8) and float arithmetic; the next entry has
// half as much red again.
int pixels_not_first_entry(const cs::Image& heat, std::string& first) {
    int count = 0;
    for (int y = 0; y < heat.height(); ++y) {
        for (int x = 0; x < heat.width(); ++x) {
            const LinearRgb pixel = as_linear(heat.at(x, y));
            bool same = true;
            for (std::size_t c = 0; c < 3; ++c) {
                same = same && std::abs(pixel[c] - magma_first[c]) <= 1e-5 * magma_first[c];
            }
            if (!same) {
                if (count == 0) {
                    std::ostringstream out;
                    out.precision(9);
                    out << "pixel (" << x << ", " << y << ") is (" << pixel[0] << ", " << pixel[1]
                        << ", " << pixel[2] << "), not (" << magma_first[0] << ", "
                        << magma_first[1] << ", " << magma_first[2] << ")";
                    first = out.str();
                }
                ++count;
            }
        }
    }
    return count;
}

double luminance(const LinearRgb& rgb) {
    return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
}

} // namespace

TEST_CASE("M1-18 AC2: the heat map tests can tell magma's 256 entries apart",
          "[core][image_compare]") {
    // What the heat map tests assume of FLIP.h's map: its first entry is the one image_compare.hpp
    // describes, every entry is in [0, 1], and every two entries differ by more than twice
    // magma_tolerance in some channel, so a pixel within it of one entry is far from every other.
    CHECK(FLIP::MapMagma[0].x == 0.001462f);
    CHECK(FLIP::MapMagma[0].y == 0.000466f);
    CHECK(FLIP::MapMagma[0].z == 0.013866f);

    const std::array<LinearRgb, 256>& map = magma_linear();
    double closest = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < map.size(); ++i) {
        for (std::size_t j = i + 1; j < map.size(); ++j) {
            closest = std::min(closest, channel_distance(map[i], map[j]));
        }
    }
    INFO("closest pair of entries: " << closest);
    CHECK(closest > 2.0 * magma_tolerance);
    int outside = 0;
    for (const LinearRgb& entry : map) {
        for (const double channel : entry) {
            outside += channel >= 0.0 && channel <= 1.0 ? 0 : 1;
        }
    }
    CHECK(outside == 0);
    // Magma's luminance rises with its index, from near black to pale yellow, which the test of a
    // brighter region below relies on.
    int falling = 0;
    for (std::size_t i = 1; i < map.size(); ++i) {
        falling += luminance(map[i]) > luminance(map[i - 1]) ? 0 : 1;
    }
    CHECK(falling == 0);
}

TEST_CASE("M1-18 AC2: flip_heat_map is the size of its inputs", "[core][image_compare]") {
    // Non-square sizes catch a transposed width and height; 1 x n and n x 1 are narrower than
    // FLIP's filters in one direction; 128 x 72 is the size of the skeleton golden image.
    const std::pair<int, int> size = GENERATE(
        values<std::pair<int, int>>({{48, 40}, {40, 48}, {1, 1}, {1, 9}, {9, 1}, {128, 72}}));
    const int width = size.first;
    const int height = size.second;
    CAPTURE(width, height);
    const cs::Image reference = random_image(width, height, 70, 10.0f);
    const cs::Image test = random_image(width, height, 71, 10.0f);

    const cs::Image heat = cs::flip_heat_map(reference, test);
    CHECK(heat.width() == width);
    CHECK(heat.height() == height);
}

TEST_CASE("M1-18 AC2: flip_heat_map of identical images is magma's first entry, as linear RGB, "
          "at every pixel",
          "[core][image_compare]") {
    // compare gives a mean FLIP of exactly 0 for identical images (above), so the error is 0 at
    // every pixel, which takes entry round(255 * 0) = 0.
    const cs::Image reference = random_image(48, 40, 72, 10.0f);
    const cs::Image copy = reference;

    for (const cs::Image& heat :
         {cs::flip_heat_map(reference, copy), cs::flip_heat_map(reference, reference)}) {
        REQUIRE(heat.width() == 48);
        REQUIRE(heat.height() == 40);
        std::string first;
        const int different = pixels_not_first_entry(heat, first);
        INFO(first);
        CHECK(different == 0);
    }
}

TEST_CASE("M1-18 AC2: flip_heat_map of two black images is magma's first entry at every pixel",
          "[core][image_compare]") {
    // A black reference leaves FLIP no exposure to find from it; compare then finds it from the
    // test image, black as well, and still says 0. The heat map must agree: the first entry, not
    // NaN.
    const cs::Image black(side, side);
    REQUIRE(cs::compare(black, black).mean_flip == 0.0);

    const cs::Image heat = cs::flip_heat_map(black, black);
    REQUIRE(heat.width() == side);
    REQUIRE(heat.height() == side);
    std::string first;
    const int different = pixels_not_first_entry(heat, first);
    INFO(first);
    CHECK(different == 0);
}

TEST_CASE("M1-18 AC2: the heat map shows the per-pixel FLIP error whose mean compare reports",
          "[core][image_compare]") {
    SECTION("random images") {
        check_heat_map_is_flip_error(random_image(48, 40, 73, 10.0f),
                                     random_image(48, 40, 74, 10.0f));
    }
    SECTION("one pixel changed") {
        const cs::Image reference = uniform(side, side, {base, base, base});
        cs::Image test = reference;
        test.at(17, 42) = {changed, changed, changed};
        check_heat_map_is_flip_error(reference, test);
    }
    SECTION("every pixel changed") {
        check_heat_map_is_flip_error(uniform(side, side, {base, base, base}),
                                     uniform(side, side, {changed, changed, changed}));
    }
    SECTION("the committed pair: a wall, a painting and a lamp, in nits") {
        const std::filesystem::path data =
            std::filesystem::path(CS_SOURCE_DIR) / "libs/core/tests/data";
        REQUIRE(std::filesystem::exists(data / "flip_reference.exr"));
        REQUIRE(std::filesystem::exists(data / "flip_test.exr"));
        check_heat_map_is_flip_error(cs::read_exr(data / "flip_reference.exr"),
                                     cs::read_exr(data / "flip_test.exr"));
    }
    SECTION("a black reference, whose exposures come from the test image") {
        check_heat_map_is_flip_error(cs::Image(side, side), random_image(side, side, 75, 100.0f));
    }
    SECTION("a NaN, an infinity and a negative value in the test image") {
        const cs::Image reference = random_image(side, side, 76, 10.0f);
        cs::Image test = reference;
        test.at(3, 4).g = std::numeric_limits<float>::quiet_NaN();
        test.at(10, 10).r = std::numeric_limits<float>::infinity();
        test.at(20, 30) = {-base, -base, -base};
        check_heat_map_is_flip_error(reference, test);
    }
    SECTION("images darker than FLIP's median floor") {
        check_heat_map_is_flip_error(uniform(side, side, {1e-8f, 1e-8f, 1e-8f}),
                                     cs::Image(side, side));
    }
    SECTION("1 x 1 images") {
        check_heat_map_is_flip_error(uniform(1, 1, {base, base, base}),
                                     uniform(1, 1, {changed, base, base}));
    }
}

TEST_CASE("M1-18 AC2: a clearly different region is brighter in the heat map than an identical "
          "one",
          "[core][image_compare]") {
    // A 64 x 64 image whose test image is four times as bright in the 16 x 16 block [40, 56) x
    // [40, 56). FLIP judges a pixel by its neighbourhood: its widest filter, the spatial one,
    // reaches ceil(3 sqrt(0.04 / (2 pi^2)) 67.02) = 10 pixels at 67 pixels per degree (FLIP.h,
    // calculateSpatialFilterRadius), its feature filters ceil(3 * 0.5 * 0.082 * 67.02) = 9. Every
    // pixel of [0, 24) x [0, 24) is at least 16 pixels from the block, so around it the two images
    // are the same, its error is 0, and it is magma's first entry.
    constexpr int size = 64;
    const cs::Image reference = random_image(size, size, 77, 10.0f);
    cs::Image test = reference;
    for (int y = 40; y < 56; ++y) {
        for (int x = 40; x < 56; ++x) {
            cs::Rgb& p = test.at(x, y);
            p = {4.0f * p.r, 4.0f * p.g, 4.0f * p.b};
        }
    }

    const cs::Image heat = cs::flip_heat_map(reference, test);
    REQUIRE(heat.width() == size);
    REQUIRE(heat.height() == size);
    const HeatMapIndices indices = read_heat_map(heat);
    INFO(indices.first_problem);
    REQUIRE(indices.not_magma == 0);
    const auto index_at = [&](int x, int y) {
        return indices.index[static_cast<std::size_t>(y * size + x)];
    };

    int identical_not_first = 0;
    for (int y = 0; y < 24; ++y) {
        for (int x = 0; x < 24; ++x) {
            identical_not_first += index_at(x, y) == 0 ? 0 : 1;
        }
    }
    CHECK(identical_not_first == 0);

    // The block's centre is a higher entry than a pixel of the identical region and, as magma's
    // luminance rises with its index, brighter.
    CAPTURE(index_at(48, 48), index_at(10, 10));
    CHECK(index_at(48, 48) > index_at(10, 10));
    CHECK(luminance(as_linear(heat.at(48, 48))) > luminance(as_linear(heat.at(10, 10))));

    // So is every pixel of the block, which differs everywhere.
    int block_first = 0;
    for (int y = 40; y < 56; ++y) {
        for (int x = 40; x < 56; ++x) {
            block_first += index_at(x, y) > 0 ? 0 : 1;
        }
    }
    CHECK(block_first == 0);
}

TEST_CASE("M1-18 AC2: every channel of the heat map is in [0, 1], whatever the inputs",
          "[core][image_compare]") {
    // FLIP sees negative and non-finite values as 0 (image_compare.hpp); a heat map of images
    // holding them is still made of magma's colours. Bright values against black push the errors
    // towards 1, the top of the map. 65504 is the largest half float, the most a reference holds.
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr float inf = std::numeric_limits<float>::infinity();
    SECTION("NaN, infinite, negative and very bright pixels in both images") {
        cs::Image reference = random_image(48, 40, 78, 100.0f);
        cs::Image test = random_image(48, 40, 79, 100.0f);
        reference.at(1, 1) = {nan, inf, -1.0f};
        reference.at(47, 39) = {65504.0f, 65504.0f, 65504.0f};
        test.at(2, 2) = {-inf, nan, 1e6f};
        test.at(30, 20) = {65504.0f, -65504.0f, 0.0f};
        check_heat_map_is_flip_error(reference, test);
        check_heat_map_is_flip_error(test, reference);
    }
    SECTION("a black reference against a thousand nits") {
        check_heat_map_is_flip_error(cs::Image(side, side),
                                     uniform(side, side, {1000.0f, 1000.0f, 1000.0f}));
    }
    SECTION("a thousand nits against black") {
        check_heat_map_is_flip_error(uniform(side, side, {1000.0f, 1000.0f, 1000.0f}),
                                     cs::Image(side, side));
    }
}

TEST_CASE("M1-18 AC2: flip_heat_map refuses images of different sizes", "[core][image_compare]") {
    CHECK_THROWS_AS(cs::flip_heat_map(cs::Image(64, 48), cs::Image(32, 17)), std::invalid_argument);
    CHECK_THROWS_AS(cs::flip_heat_map(cs::Image(64, 64), cs::Image(64, 63)), std::invalid_argument);
    CHECK_THROWS_AS(cs::flip_heat_map(cs::Image(64, 64), cs::Image(63, 64)), std::invalid_argument);
    // Transposed: the same number of pixels is not the same size.
    CHECK_THROWS_AS(cs::flip_heat_map(cs::Image(64, 32), cs::Image(32, 64)), std::invalid_argument);
    CHECK_THROWS_AS(cs::flip_heat_map(cs::Image(1, 1), cs::Image(1, 2)), std::invalid_argument);
}
