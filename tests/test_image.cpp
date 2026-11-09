#include <cs/core/image.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <stdexcept>

TEST_CASE("a new image is black and has the requested size", "[image]") {
    const cs::Image image(4, 3);
    CHECK(image.width() == 4);
    CHECK(image.height() == 3);
    CHECK(image.at(3, 2).r == 0.0f);
    CHECK(image.at(3, 2).g == 0.0f);
    CHECK(image.at(3, 2).b == 0.0f);
}

TEST_CASE("an image refuses sizes that are not positive", "[image]") {
    CHECK_THROWS_AS(cs::Image(0, 10), std::invalid_argument);
    CHECK_THROWS_AS(cs::Image(10, -1), std::invalid_argument);
}

TEST_CASE("linear values become 8-bit sRGB values", "[image]") {
    CHECK(cs::linear_to_srgb8(0.0f) == 0);
    CHECK(cs::linear_to_srgb8(1.0f) == 255);

    // sRGB is not linear: half the light is stored as 188, not 128.
    CHECK(cs::linear_to_srgb8(0.5f) == 188);

    // Values outside [0, 1], and NaN, are clamped.
    CHECK(cs::linear_to_srgb8(4.0f) == 255);
    CHECK(cs::linear_to_srgb8(-1.0f) == 0);
    CHECK(cs::linear_to_srgb8(std::numeric_limits<float>::quiet_NaN()) == 0);
}
