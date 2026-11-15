// SPDX-License-Identifier: Apache-2.0

// Tests of texture.hpp (M1-14): PNG and JPEG files load into linear light with the exact sRGB
// curve, mips average in linear light, and images too large to be safe are refused before they
// are decoded.

#include <cs/assets/texture.hpp>
#include <cs/core/image.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

// The tests write their PNG and JPEG files in memory with stb_image_write. STATIC keeps its
// functions inside this file, apart from core's copy.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;

namespace {

using Bytes = std::vector<std::byte>;

void append_bytes(void* context, void* data, int size) {
    auto* out = static_cast<Bytes*>(context);
    const auto* begin = static_cast<const std::byte*>(data);
    out->insert(out->end(), begin, begin + size);
}

// An 8-bit PNG of width x height pixels with `channels` channels (1 grey, 3 RGB, 4 RGBA).
Bytes png8(int width, int height, int channels, const std::vector<std::uint8_t>& values) {
    Bytes out;
    REQUIRE(stbi_write_png_to_func(append_bytes, &out, width, height, channels, values.data(),
                                   width * channels) != 0);
    return out;
}

Bytes jpeg(int width, int height, const std::vector<std::uint8_t>& rgb) {
    Bytes out;
    REQUIRE(stbi_write_jpg_to_func(append_bytes, &out, width, height, 3, rgb.data(), 100) != 0);
    return out;
}

// stb_image_write writes only 8-bit PNGs, so 16-bit ones are put together here: an IHDR, one
// IDAT of uncompressed (stored) deflate blocks, and an IEND.
std::uint32_t crc32(std::span<const std::uint8_t> data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::uint8_t byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

void put_u32_be(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(v >> shift));
    }
}

void put_chunk(std::vector<std::uint8_t>& out, const char (&type)[5],
               const std::vector<std::uint8_t>& data) {
    put_u32_be(out, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> typed(type, type + 4);
    typed.insert(typed.end(), data.begin(), data.end());
    out.insert(out.end(), typed.begin(), typed.end());
    put_u32_be(out, crc32(typed));
}

Bytes to_bytes(const std::vector<std::uint8_t>& v) {
    Bytes out(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) {
        out[i] = static_cast<std::byte>(v[i]);
    }
    return out;
}

std::vector<std::uint8_t> ihdr(std::uint32_t width, std::uint32_t height, std::uint8_t depth) {
    std::vector<std::uint8_t> data;
    put_u32_be(data, width);
    put_u32_be(data, height);
    data.insert(data.end(), {depth, 2, 0, 0, 0}); // colour type 2 (RGB), deflate, no interlace
    return data;
}

const std::array<std::uint8_t, 8> png_signature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

// A 16-bit RGB PNG; rgb16 holds width * height * 3 values, row by row.
Bytes png16(int width, int height, const std::vector<std::uint16_t>& rgb16) {
    std::vector<std::uint8_t> raw;
    for (int y = 0; y < height; ++y) {
        raw.push_back(0); // filter type None
        for (int i = 0; i < width * 3; ++i) {
            const std::uint16_t v = rgb16[static_cast<std::size_t>(y * width * 3 + i)];
            raw.push_back(static_cast<std::uint8_t>(v >> 8));
            raw.push_back(static_cast<std::uint8_t>(v & 0xFF));
        }
    }
    REQUIRE(raw.size() <= 65535); // one stored block

    std::vector<std::uint8_t> zlib = {0x78, 0x01, 0x01}; // zlib header; final stored block
    const auto len = static_cast<std::uint16_t>(raw.size());
    zlib.insert(zlib.end(),
                {static_cast<std::uint8_t>(len & 0xFF), static_cast<std::uint8_t>(len >> 8),
                 static_cast<std::uint8_t>(~len & 0xFF),
                 static_cast<std::uint8_t>((~len >> 8) & 0xFF)});
    zlib.insert(zlib.end(), raw.begin(), raw.end());
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::uint8_t byte : raw) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    put_u32_be(zlib, (b << 16) | a);

    std::vector<std::uint8_t> out(png_signature.begin(), png_signature.end());
    put_chunk(out, "IHDR",
              ihdr(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 16));
    put_chunk(out, "IDAT", zlib);
    put_chunk(out, "IEND", {});
    return to_bytes(out);
}

// A PNG that is only a signature and a header claiming width x height: enough for the size
// check, which must refuse it before decoding anything.
Bytes png_header_only(std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint8_t> out(png_signature.begin(), png_signature.end());
    put_chunk(out, "IHDR", ihdr(width, height, 8));
    put_chunk(out, "IEND", {});
    return to_bytes(out);
}

// The exact sRGB curve, written out independently of the implementation.
double reference_srgb_to_linear(double c) {
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

} // namespace

TEST_CASE("srgb_to_linear is the exact sRGB curve", "[assets][texture][accuracy]") {
    // Tolerance 1e-4 is the issue's; the analytic value is 0.502886.
    CHECK(cs::assets::srgb_to_linear(188.0 / 255.0) == Catch::Approx(0.5029).margin(1e-4));
    CHECK(cs::assets::srgb_to_linear(0.0) == 0.0);
    CHECK(cs::assets::srgb_to_linear(1.0) == Catch::Approx(1.0).margin(1e-12));
    // The linear toe meets the power curve at 0.04045 (continuity within double rounding).
    CHECK(cs::assets::srgb_to_linear(0.04045) == Catch::Approx(0.04045 / 12.92).margin(1e-7));
    for (int v = 0; v <= 255; ++v) {
        CHECK(cs::assets::srgb_to_linear(v / 255.0) ==
              Catch::Approx(reference_srgb_to_linear(v / 255.0)).margin(1e-12));
    }
}

TEST_CASE("An 8-bit PNG loads into linear light", "[assets][texture][accuracy]") {
    // Float storage: 1e-6 covers the rounding of a double near 0.5 to float.
    const auto texture = cs::assets::load_texture_from_memory(png8(1, 1, 3, {188, 188, 188}));
    REQUIRE(texture.width() == 1);
    REQUIRE(texture.height() == 1);
    const cs::Rgb p = texture.levels[0].at(0, 0);
    CHECK(p.r == Catch::Approx(0.5029).margin(1e-4));
    CHECK(p.r == Catch::Approx(reference_srgb_to_linear(188.0 / 255.0)).margin(1e-6));
    CHECK(p.g == p.r);
    CHECK(p.b == p.r);

    SECTION("channels stay in their places, and rows go from the top") {
        const auto t = cs::assets::load_texture_from_memory(
            png8(2, 2, 3, {255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0}));
        const cs::Image& image = t.levels[0];
        CHECK(image.at(0, 0).r == 1.0f);
        CHECK(image.at(0, 0).g == 0.0f);
        CHECK(image.at(1, 0).g == 1.0f);
        CHECK(image.at(0, 1).b == 1.0f);
        CHECK(image.at(1, 1).r == 0.0f);
        CHECK(image.at(1, 1).g == 0.0f);
        CHECK(image.at(1, 1).b == 0.0f);
    }

    SECTION("grey becomes R = G = B") {
        const auto t = cs::assets::load_texture_from_memory(png8(1, 1, 1, {188}));
        CHECK(t.levels[0].at(0, 0).r == Catch::Approx(0.5029).margin(1e-4));
        CHECK(t.levels[0].at(0, 0).g == t.levels[0].at(0, 0).r);
        CHECK(t.levels[0].at(0, 0).b == t.levels[0].at(0, 0).r);
    }

    SECTION("alpha is dropped") {
        const auto t = cs::assets::load_texture_from_memory(png8(1, 1, 4, {188, 0, 255, 0}));
        CHECK(t.levels[0].at(0, 0).r == Catch::Approx(0.5029).margin(1e-4));
        CHECK(t.levels[0].at(0, 0).g == 0.0f);
        CHECK(t.levels[0].at(0, 0).b == 1.0f);
    }
}

TEST_CASE("A 16-bit PNG loads with its 16 bits", "[assets][texture][accuracy]") {
    // 188 * 257 is 8-bit 188 widened exactly, so it is the same value.
    const auto t =
        cs::assets::load_texture_from_memory(png16(2, 1, {188 * 257, 32768, 65535, 32769, 0, 1}));
    const cs::Image& image = t.levels[0];
    CHECK(image.at(0, 0).r == Catch::Approx(0.5029).margin(1e-4));
    CHECK(image.at(0, 0).g ==
          Catch::Approx(reference_srgb_to_linear(32768.0 / 65535.0)).margin(1e-6));
    CHECK(image.at(0, 0).b == 1.0f);
    // One 16-bit step apart: an 8-bit decode would make these equal.
    CHECK(image.at(1, 0).r != image.at(0, 0).g);
    CHECK(image.at(1, 0).r ==
          Catch::Approx(reference_srgb_to_linear(32769.0 / 65535.0)).margin(1e-6));
    CHECK(image.at(1, 0).g == 0.0f);
    CHECK(image.at(1, 0).b == Catch::Approx(1.0 / 65535.0 / 12.92).margin(1e-9));
}

TEST_CASE("A JPEG loads into linear light", "[assets][texture][accuracy]") {
    // JPEG is lossy: a flat grey 188 at quality 100 decodes to 188 give or take one code, and
    // one code near 188 is about 0.0045 in linear light.
    std::vector<std::uint8_t> rgb(16 * 16 * 3, 188);
    const auto t = cs::assets::load_texture_from_memory(jpeg(16, 16, rgb));
    REQUIRE(t.width() == 16);
    REQUIRE(t.height() == 16);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            CHECK(t.levels[0].at(x, y).r == Catch::Approx(0.5029).margin(0.005));
            CHECK(t.levels[0].at(x, y).g == Catch::Approx(0.5029).margin(0.005));
            CHECK(t.levels[0].at(x, y).b == Catch::Approx(0.5029).margin(0.005));
        }
    }
}

TEST_CASE("Mips average in linear light", "[assets][texture][accuracy]") {
    // A 2x2 checkerboard of sRGB 0 and 255 is linear 0 and 1 exactly, so its average is 0.5
    // exactly; averaging the sRGB values would give 128, linear 0.216.
    const auto t = cs::assets::load_texture_from_memory(
        png8(2, 2, 3, {0, 0, 0, 255, 255, 255, 255, 255, 255, 0, 0, 0}));
    REQUIRE(t.levels.size() == 2);
    REQUIRE(t.levels[1].width() == 1);
    REQUIRE(t.levels[1].height() == 1);
    CHECK(t.levels[1].at(0, 0).r == 0.5f);
    CHECK(t.levels[1].at(0, 0).g == 0.5f);
    CHECK(t.levels[1].at(0, 0).b == 0.5f);
}

TEST_CASE("Mip levels of odd sizes are area averages", "[assets][texture][accuracy]") {
    SECTION("3 x 1 to 1 x 1 is the mean of all three") {
        cs::Image image(3, 1);
        image.at(0, 0) = {0.0f, 1.0f, 2.0f};
        image.at(1, 0) = {3.0f, 1.0f, 2.0f};
        image.at(2, 0) = {6.0f, 1.0f, 2.0f};
        const auto levels = cs::assets::build_mip_chain(std::move(image));
        REQUIRE(levels.size() == 2);
        CHECK(levels[1].at(0, 0).r == Catch::Approx(3.0f).margin(1e-6));
        CHECK(levels[1].at(0, 0).g == Catch::Approx(1.0f).margin(1e-6));
        CHECK(levels[1].at(0, 0).b == Catch::Approx(2.0f).margin(1e-6));
    }

    SECTION("every level keeps the mean of the image") {
        // Area averaging conserves the total light: the mean of each level equals the mean of
        // level 0. 1e-5 relative covers float sums over 37 x 23 pixels.
        cs::Image image(37, 23);
        double sum = 0.0;
        for (int y = 0; y < 23; ++y) {
            for (int x = 0; x < 37; ++x) {
                const float v = static_cast<float>((x * 7 + y * 13) % 17) / 4.0f;
                image.at(x, y) = {v, 2.0f * v, 0.5f};
                sum += v;
            }
        }
        const double mean = sum / (37.0 * 23.0);
        const auto levels = cs::assets::build_mip_chain(std::move(image));
        REQUIRE(levels.size() == 6); // 37 x 23, 18 x 11, 9 x 5, 4 x 2, 2 x 1, 1 x 1
        for (const cs::Image& level : levels) {
            double level_sum = 0.0;
            for (int y = 0; y < level.height(); ++y) {
                for (int x = 0; x < level.width(); ++x) {
                    level_sum += level.at(x, y).r;
                    CHECK(level.at(x, y).b == Catch::Approx(0.5f).margin(1e-6));
                }
            }
            CHECK(level_sum / (level.width() * level.height()) ==
                  Catch::Approx(mean).epsilon(1e-5));
        }
    }
}

TEST_CASE("A 1000 x 600 image has 10 mip levels", "[assets][texture]") {
    CHECK(cs::assets::mip_level_count(1000, 600) == 10);
    CHECK(cs::assets::mip_level_count(1, 1) == 1);
    CHECK(cs::assets::mip_level_count(2, 1) == 2);
    CHECK(cs::assets::mip_level_count(1, 1024) == 11);
    CHECK(cs::assets::mip_level_count(16384, 16384) == 15);
    CHECK_THROWS_AS(cs::assets::mip_level_count(0, 5), std::invalid_argument);

    const std::vector<std::uint8_t> grey(1000 * 600, 128);
    const auto t = cs::assets::load_texture_from_memory(png8(1000, 600, 1, grey));
    REQUIRE(t.levels.size() == 10);
    const std::array<std::pair<int, int>, 10> sizes = {{{1000, 600},
                                                        {500, 300},
                                                        {250, 150},
                                                        {125, 75},
                                                        {62, 37},
                                                        {31, 18},
                                                        {15, 9},
                                                        {7, 4},
                                                        {3, 2},
                                                        {1, 1}}};
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        CHECK(t.levels[i].width() == sizes[i].first);
        CHECK(t.levels[i].height() == sizes[i].second);
    }
    // A flat image stays flat on every level.
    CHECK(t.levels[9].at(0, 0).r ==
          Catch::Approx(reference_srgb_to_linear(128.0 / 255.0)).margin(1e-6));
}

TEST_CASE("Images too large are refused with a message", "[assets][texture]") {
    SECTION("over 16384 pixels on a side") {
        CHECK_THROWS_WITH(cs::assets::load_texture_from_memory(png_header_only(16385, 1)),
                          ContainsSubstring("16385") && ContainsSubstring("16384"));
        CHECK_THROWS_WITH(cs::assets::load_texture_from_memory(png_header_only(1, 16385)),
                          ContainsSubstring("16384"));
    }

    SECTION("over 512 MiB decoded") {
        // 8192 x 8192 is within the side limit, but 768 MiB as linear float RGB.
        CHECK_THROWS_WITH(cs::assets::load_texture_from_memory(png_header_only(8192, 8192)),
                          ContainsSubstring("512 MiB"));
    }
}

TEST_CASE("Input that is not a PNG or JPEG is refused", "[assets][texture]") {
    CHECK_THROWS_AS(cs::assets::load_texture_from_memory({}), std::runtime_error);

    const std::array<std::byte, 4> junk = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    CHECK_THROWS_AS(cs::assets::load_texture_from_memory(junk), std::runtime_error);

    // A PNG cut off halfway through its pixels.
    Bytes cut = png8(16, 16, 3, std::vector<std::uint8_t>(16 * 16 * 3, 77));
    cut.resize(cut.size() / 2);
    CHECK_THROWS_AS(cs::assets::load_texture_from_memory(cut), std::runtime_error);

    // A header that claims an image but has no pixels.
    CHECK_THROWS_AS(cs::assets::load_texture_from_memory(png_header_only(4, 4)),
                    std::runtime_error);
}

TEST_CASE("load_texture reads a file", "[assets][texture]") {
    const auto dir = std::filesystem::temp_directory_path() / "cs_assets_test_texture";
    std::filesystem::create_directories(dir);
    const auto path = dir / "grey.png";
    {
        const Bytes bytes = png8(3, 2, 3, std::vector<std::uint8_t>(3 * 2 * 3, 188));
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    const auto t = cs::assets::load_texture(path);
    CHECK(t.width() == 3);
    CHECK(t.height() == 2);
    CHECK(t.levels.size() == 2);
    CHECK(t.levels[0].at(2, 1).g == Catch::Approx(0.5029).margin(1e-4));

    CHECK_THROWS_WITH(cs::assets::load_texture(dir / "missing.png"),
                      ContainsSubstring("missing.png"));
    std::filesystem::remove_all(dir);
}
