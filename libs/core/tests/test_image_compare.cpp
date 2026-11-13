// SPDX-License-Identifier: Apache-2.0

// Tests of image_compare.hpp (M1-07): the mean absolute error, the maximum error and the mean
// NVIDIA FLIP error of a test image against a reference.

#include <cs/core/image.hpp>
#include <cs/core/image_compare.hpp>
#include <cs/core/image_io.hpp>
#include <cs/core/random.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>

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
std::string size_error_message(const cs::Image& reference, const cs::Image& test) {
    try {
        (void)cs::compare(reference, test);
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    FAIL("compare did not throw std::invalid_argument for images of different sizes");
    return "";
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
