// SPDX-License-Identifier: Apache-2.0

// Tests of image_io.hpp (M1-07): linear EXR files, written as 32-bit or 16-bit float channels and
// read back, display PNGs through the exposure, tone mapping and sRGB pipeline of colour.hpp, and
// EXR input from outside, which is hostile; and (M1-18) PNGs of images that are already
// display-referred, such as the golden-image runner's FLIP heat map, through the sRGB curve alone.

#include <cs/core/colour.hpp>
#include <cs/core/image.hpp>
#include <cs/core/image_io.hpp>
#include <cs/core/random.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <glm/vec3.hpp>

// core does not define stb_image's implementation; this test executable does, to read PNGs back.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// The reference implementation's magma map, FLIP::MapMagma, for the display PNG of a heat map.
// Last, as it defines the macros Max and Min.
#include <FLIP.h>

namespace {

// A file in the system's temporary directory, named after the test that uses it, removed when the
// test ends (and before it starts, in case an earlier run crashed).
class TempFile {
  public:
    explicit TempFile(const std::string& name)
        : path_(std::filesystem::temp_directory_path() / ("cs_core_test_image_io_" + name)) {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

// A directory that does not exist, for files that cannot be written.
std::filesystem::path missing_directory() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "cs_core_test_image_io_no_such_directory";
    REQUIRE_FALSE(std::filesystem::exists(dir));
    return dir;
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file);
    std::vector<std::byte> bytes(static_cast<std::size_t>(std::filesystem::file_size(path)));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.gcount() == static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

std::uint32_t bits(float f) {
    return std::bit_cast<std::uint32_t>(f);
}

// A finite float drawn from all of float's range: random bits, drawn again while they are an
// infinity or a NaN. Subnormals, negative zero, huge and tiny values all occur.
float random_finite_bits(cs::Rng& rng) {
    for (;;) {
        const float f = std::bit_cast<float>(rng.next_u32());
        if (std::isfinite(f)) {
            return f;
        }
    }
}

// A random sign times 2^e times a mantissa in [1, 2), e in [-20, 20): values from about 1e-6 to
// 1e6, the range of a render in nits.
float random_magnitude(cs::Rng& rng) {
    const float mantissa = 1.0f + rng.next_float();
    const int exponent = static_cast<int>(rng.next_u32() % 40u) - 20;
    const float sign = (rng.next_u32() & 1u) != 0 ? -1.0f : 1.0f;
    return sign * std::ldexp(mantissa, exponent);
}

// The first pixel whose channels differ in their bit patterns, or "" if none does.
std::string first_bit_difference(const cs::Image& expected, const cs::Image& actual) {
    for (int y = 0; y < expected.height(); ++y) {
        for (int x = 0; x < expected.width(); ++x) {
            const cs::Rgb& e = expected.at(x, y);
            const cs::Rgb& a = actual.at(x, y);
            if (bits(e.r) != bits(a.r) || bits(e.g) != bits(a.g) || bits(e.b) != bits(a.b)) {
                std::ostringstream out;
                out.precision(9);
                out << "pixel (" << x << ", " << y << "): wrote (" << e.r << ", " << e.g << ", "
                    << e.b << "), read (" << a.r << ", " << a.g << ", " << a.b << ")";
                return out.str();
            }
        }
    }
    return "";
}

// --- Handmade EXR files ------------------------------------------------------------------------
//
// The smallest valid OpenEXR file there is, written byte by byte from the file layout of
// openexr.com ("The OpenEXR File Layout"), so the hostile-input tests can change one field of an
// otherwise valid header. Little-endian throughout:
//
//   magic 76 2f 31 01, version 2 with no flags (single-part scanline)
//   attributes: name \0 type \0 int32 size, value; the header ends with a \0
//     channels (chlist): per channel: name \0, int32 pixel type (2 = FLOAT), uint8 pLinear,
//       3 reserved bytes, int32 x sampling, int32 y sampling; then \0. Names are sorted.
//     compression (uint8 0 = NO_COMPRESSION), dataWindow and displayWindow (box2i: xmin ymin
//     xmax ymax), lineOrder (uint8 0 = INCREASING_Y), pixelAspectRatio (float),
//     screenWindowCenter (v2f), screenWindowWidth (float)
//   offset table: one uint64 file offset per chunk; uncompressed, a chunk is one scanline
//   chunk: int32 y, int32 data size, then each channel's values for the line, in channel order

struct Box2i {
    std::int32_t xmin;
    std::int32_t ymin;
    std::int32_t xmax;
    std::int32_t ymax;
};

class ByteWriter {
  public:
    void u8(std::uint8_t v) { bytes_.push_back(static_cast<std::byte>(v)); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            u8(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            u8(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    }
    void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }
    void str(std::string_view s) {
        for (const char c : s) {
            u8(static_cast<std::uint8_t>(c));
        }
        u8(0);
    }
    void attribute(std::string_view name, std::string_view type, std::int32_t size) {
        str(name);
        str(type);
        i32(size);
    }
    void box(const Box2i& b) {
        i32(b.xmin);
        i32(b.ymin);
        i32(b.xmax);
        i32(b.ymax);
    }
    std::size_t size() const { return bytes_.size(); }
    std::vector<std::byte>& bytes() { return bytes_; }

  private:
    std::vector<std::byte> bytes_;
};

// An uncompressed single-part scanline EXR with 32-bit float channels named `channels` (sorted,
// as EXR requires) over `window`. If `pixels` is not empty, pixels[c][y * width + x] is channel c
// of pixel (x, y), and the offset table and the scanlines follow the header; if it is empty, the
// file ends after the header, as a header that a reader must refuse before it reads any pixels.
std::vector<std::byte> handmade_exr(const Box2i& window, const std::vector<std::string>& channels,
                                    const std::vector<std::vector<float>>& pixels) {
    ByteWriter w;
    w.u8(0x76);
    w.u8(0x2f);
    w.u8(0x31);
    w.u8(0x01);
    w.u32(2);

    std::int32_t chlist_size = 1;
    for (const std::string& name : channels) {
        chlist_size += static_cast<std::int32_t>(name.size()) + 1 + 16;
    }
    w.attribute("channels", "chlist", chlist_size);
    for (const std::string& name : channels) {
        w.str(name);
        w.i32(2); // FLOAT
        w.u8(0);  // pLinear
        w.u8(0);
        w.u8(0);
        w.u8(0);
        w.i32(1); // x sampling
        w.i32(1); // y sampling
    }
    w.u8(0);

    w.attribute("compression", "compression", 1);
    w.u8(0);
    w.attribute("dataWindow", "box2i", 16);
    w.box(window);
    w.attribute("displayWindow", "box2i", 16);
    w.box(window);
    w.attribute("lineOrder", "lineOrder", 1);
    w.u8(0);
    w.attribute("pixelAspectRatio", "float", 4);
    w.f32(1.0f);
    w.attribute("screenWindowCenter", "v2f", 8);
    w.f32(0.0f);
    w.f32(0.0f);
    w.attribute("screenWindowWidth", "float", 4);
    w.f32(1.0f);
    w.u8(0);

    if (!pixels.empty()) {
        const int width = window.xmax - window.xmin + 1;
        const int height = window.ymax - window.ymin + 1;
        const auto line_bytes =
            static_cast<std::int32_t>(width * 4 * static_cast<int>(channels.size()));
        const std::size_t table_end = w.size() + 8 * static_cast<std::size_t>(height);
        for (int y = 0; y < height; ++y) {
            w.u64(table_end + static_cast<std::size_t>(y) * (8 + line_bytes));
        }
        for (int y = 0; y < height; ++y) {
            w.i32(window.ymin + y);
            w.i32(line_bytes);
            for (const std::vector<float>& channel : pixels) {
                for (int x = 0; x < width; ++x) {
                    w.f32(channel[static_cast<std::size_t>(y * width + x)]);
                }
            }
        }
    }
    return std::move(w.bytes());
}

const std::vector<std::string> bgr = {"B", "G", "R"};

// The 2 x 2 image the handmade files hold: R, G and B of pixel (x, y) are 1 + x + 2 y, 10 times
// that and 100 times that, all exact in float.
const std::vector<std::vector<float>> bgr_2x2 = {
    {100.0f, 200.0f, 300.0f, 400.0f}, // B
    {10.0f, 20.0f, 30.0f, 40.0f},     // G
    {1.0f, 2.0f, 3.0f, 4.0f},         // R
};

void check_2x2(const cs::Image& image) {
    REQUIRE(image.width() == 2);
    REQUIRE(image.height() == 2);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            const float v = static_cast<float>(1 + x + 2 * y);
            CHECK(image.at(x, y).r == v);
            CHECK(image.at(x, y).g == 10.0f * v);
            CHECK(image.at(x, y).b == 100.0f * v);
        }
    }
}

} // namespace

// --- 32-bit float EXR ----------------------------------------------------------------------------

TEST_CASE("AC1: write_exr then read_exr of a 37x23 float32 image is bit-exact",
          "[core][image_io]") {
    cs::Image image(37, 23);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            cs::Rng rng(cs::hash_seed(50, static_cast<std::uint32_t>(x),
                                      static_cast<std::uint32_t>(y), 0, 0));
            image.at(x, y) = {random_magnitude(rng), random_finite_bits(rng), rng.next_float()};
        }
    }
    // The values the criterion names, and the edges of float: zero and negative zero, the
    // smallest subnormal, the largest and most negative finite floats.
    image.at(0, 0) = {1e4f, 1e-4f, 0.0f};
    image.at(36, 22) = {-1e4f, -1e-4f, -0.0f};
    image.at(36, 0) = {std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::max(),
                       std::numeric_limits<float>::lowest()};
    image.at(0, 22) = {-0.5f, 0.0f, 65504.0f};

    const TempFile file("float32_round_trip.exr");
    cs::write_exr(image, file.path(), cs::ExrChannels::float32);
    const cs::Image read = cs::read_exr(file.path());

    REQUIRE(read.width() == 37);
    REQUIRE(read.height() == 23);
    INFO(first_bit_difference(image, read));
    CHECK(first_bit_difference(image, read).empty());
    // Negative zero is a different bit pattern from zero; check it explicitly.
    CHECK(std::signbit(read.at(36, 22).b));
}

TEST_CASE("AC1: write_exr writes 32-bit float channels by default", "[core][image_io]") {
    cs::Image image(5, 3);
    cs::Rng rng(cs::hash_seed(51, 0, 0, 0, 0));
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 5; ++x) {
            image.at(x, y) = {random_magnitude(rng), random_magnitude(rng), random_magnitude(rng)};
        }
    }
    const TempFile file("default_channels.exr");
    cs::write_exr(image, file.path());
    const cs::Image read = cs::read_exr(file.path());
    REQUIRE(read.width() == 5);
    REQUIRE(read.height() == 3);
    INFO(first_bit_difference(image, read));
    CHECK(first_bit_difference(image, read).empty());
}

TEST_CASE("AC1: a 1x1 EXR round trip is bit-exact", "[core][image_io]") {
    cs::Image image(1, 1);
    image.at(0, 0) = {0.1f, 1e-4f, 1e4f};
    const TempFile file("one_pixel.exr");
    cs::write_exr(image, file.path());
    const cs::Image read = cs::read_exr(file.path());
    REQUIRE(read.width() == 1);
    REQUIRE(read.height() == 1);
    CHECK(first_bit_difference(image, read).empty());
}

TEST_CASE("AC1: read_exr_from_memory reads the bytes write_exr wrote", "[core][image_io]") {
    cs::Image image(7, 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 7; ++x) {
            image.at(x, y) = {static_cast<float>(x), static_cast<float>(y), 0.25f};
        }
    }
    const TempFile file("from_memory.exr");
    cs::write_exr(image, file.path());
    const std::vector<std::byte> bytes = read_bytes(file.path());
    const cs::Image read = cs::read_exr_from_memory(bytes);
    REQUIRE(read.width() == 7);
    REQUIRE(read.height() == 4);
    CHECK(first_bit_difference(image, read).empty());
}

TEST_CASE("write_exr throws std::runtime_error when the file cannot be written",
          "[core][image_io]") {
    const cs::Image image(2, 2);
    const std::filesystem::path path = missing_directory() / "image.exr";
    CHECK_THROWS_AS(cs::write_exr(image, path), std::runtime_error);
    CHECK_THROWS_AS(cs::write_exr(image, path, cs::ExrChannels::half), std::runtime_error);
}

// --- 16-bit half EXR -----------------------------------------------------------------------------

TEST_CASE("AC6: a half EXR round trip of values in [6.2e-5, 65504] is within relative 2^-11",
          "[core][image_io][accuracy]") {
    // Half floats have 10 stored mantissa bits. Rounding to nearest moves a normal value by at most
    // half a unit in the last place, 2^-11 of the power of two below it, so by at most 2^-11 of
    // itself. Halves are normal from 2^-14 = 6.1035e-5; above that up to the largest half, 65504,
    // the bound holds everywhere (65504 itself is a half, exactly).
    static constexpr double low = 6.2e-5;
    static constexpr double high = 65504.0;
    cs::Image image(64, 32);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            cs::Rng rng(cs::hash_seed(52, static_cast<std::uint32_t>(x),
                                      static_cast<std::uint32_t>(y), 0, 0));
            // Log-uniform in [low, high], with a random sign.
            const auto draw = [&rng] {
                const double t = rng.next_double();
                const double v = std::min(high, std::max(low, low * std::pow(high / low, t)));
                return static_cast<float>((rng.next_u32() & 1u) != 0 ? -v : v);
            };
            const float r = draw();
            const float g = draw();
            const float b = draw();
            image.at(x, y) = {r, g, b};
        }
    }
    image.at(0, 0) = {static_cast<float>(low), static_cast<float>(high), -static_cast<float>(high)};
    image.at(1, 0) = {-static_cast<float>(low), 1.0f, 0.5f};

    const TempFile file("half_round_trip.exr");
    cs::write_exr(image, file.path(), cs::ExrChannels::half);
    const cs::Image read = cs::read_exr(file.path());
    REQUIRE(read.width() == 64);
    REQUIRE(read.height() == 32);

    int failures = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const cs::Rgb& e = image.at(x, y);
            const cs::Rgb& a = read.at(x, y);
            const std::array<std::array<float, 2>, 3> pairs = {
                {{e.r, a.r}, {e.g, a.g}, {e.b, a.b}}};
            for (const auto& [wrote, got] : pairs) {
                const double error = std::abs(static_cast<double>(got) - wrote);
                if (!(error <= 0x1p-11 * std::abs(static_cast<double>(wrote)))) {
                    if (failures == 0) {
                        UNSCOPED_INFO("pixel (" << x << ", " << y << "): wrote " << wrote
                                                << ", read " << got);
                    }
                    ++failures;
                }
            }
        }
    }
    CHECK(failures == 0);
    // Values a half holds exactly come back exactly.
    CHECK(read.at(0, 0).g == 65504.0f);
    CHECK(read.at(0, 0).b == -65504.0f);
    CHECK(read.at(1, 0).g == 1.0f);
    CHECK(read.at(1, 0).b == 0.5f);
}

TEST_CASE("AC6: half EXR stores values beyond 65504 as +-65504, keeps NaN NaN and zero zero",
          "[core][image_io]") {
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr float inf = std::numeric_limits<float>::infinity();
    cs::Image image(4, 2);
    // Row 0: beyond the largest half, positive and negative. 65505 rounds to 65504 anyway; 65520
    // is where rounding to nearest would give infinity.
    image.at(0, 0) = {65505.0f, 65520.0f, 1e5f};
    image.at(1, 0) = {1e30f, std::numeric_limits<float>::max(), inf};
    image.at(2, 0) = {-65520.0f, -1e30f, -inf};
    image.at(3, 0) = {-std::numeric_limits<float>::max(), -1e5f, 70000.0f};
    // Row 1: NaN in each channel, and zero.
    image.at(0, 1) = {nan, 0.25f, 0.0f};
    image.at(1, 1) = {0.25f, nan, 0.0f};
    image.at(2, 1) = {0.25f, 0.0f, nan};
    image.at(3, 1) = {nan, nan, nan};

    const TempFile file("half_special.exr");
    cs::write_exr(image, file.path(), cs::ExrChannels::half);
    const cs::Image read = cs::read_exr(file.path());
    REQUIRE(read.width() == 4);
    REQUIRE(read.height() == 2);

    CHECK(read.at(0, 0).r == 65504.0f);
    CHECK(read.at(0, 0).g == 65504.0f);
    CHECK(read.at(0, 0).b == 65504.0f);
    CHECK(read.at(1, 0).r == 65504.0f);
    CHECK(read.at(1, 0).g == 65504.0f);
    // An infinity is beyond the largest half too; the header promises a reference stays finite.
    CHECK(read.at(1, 0).b == 65504.0f);
    CHECK(read.at(2, 0).r == -65504.0f);
    CHECK(read.at(2, 0).g == -65504.0f);
    CHECK(read.at(2, 0).b == -65504.0f);
    CHECK(read.at(3, 0).r == -65504.0f);
    CHECK(read.at(3, 0).g == -65504.0f);
    CHECK(read.at(3, 0).b == 65504.0f);

    CHECK(std::isnan(read.at(0, 1).r));
    CHECK(read.at(0, 1).g == 0.25f);
    CHECK(read.at(0, 1).b == 0.0f);
    CHECK(std::isnan(read.at(1, 1).g));
    CHECK(read.at(1, 1).r == 0.25f);
    CHECK(std::isnan(read.at(2, 1).b));
    CHECK(read.at(2, 1).g == 0.0f);
    CHECK(std::isnan(read.at(3, 1).r));
    CHECK(std::isnan(read.at(3, 1).g));
    CHECK(std::isnan(read.at(3, 1).b));
}

TEST_CASE("AC6: a half EXR is smaller than a float EXR of the same image", "[core][image_io]") {
    // Random mantissas do not compress, so the half file should be about half the size.
    cs::Image image(64, 64);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            cs::Rng rng(cs::hash_seed(53, static_cast<std::uint32_t>(x),
                                      static_cast<std::uint32_t>(y), 0, 0));
            image.at(x, y) = {100.0f * rng.next_float(), 100.0f * rng.next_float(),
                              100.0f * rng.next_float()};
        }
    }
    const TempFile float_file("size_float.exr");
    const TempFile half_file("size_half.exr");
    cs::write_exr(image, float_file.path(), cs::ExrChannels::float32);
    cs::write_exr(image, half_file.path(), cs::ExrChannels::half);
    const auto float_bytes = std::filesystem::file_size(float_file.path());
    const auto half_bytes = std::filesystem::file_size(half_file.path());
    INFO("float32: " << float_bytes << " bytes, half: " << half_bytes << " bytes");
    CHECK(half_bytes < float_bytes);
}

// --- PNG ---------------------------------------------------------------------------------------

namespace {

// What write_png should store for one channel: exposure_scale(ev100), pbr_neutral and
// linear_to_srgb8, computed here in double. write_png may compute in float; the difference is far
// below one 8-bit step except where a value lies on a rounding boundary, which the tolerance of 1
// from the criterion covers.
int expected_srgb8(const cs::Rgb& rgb, double ev100, int channel) {
    const glm::dvec3 exposed = glm::dvec3(rgb.r, rgb.g, rgb.b) * cs::exposure_scale(ev100);
    const glm::dvec3 display = cs::pbr_neutral(exposed);
    return cs::linear_to_srgb8(static_cast<float>(display[channel]));
}

struct StbiFree {
    void operator()(unsigned char* data) const { stbi_image_free(data); }
};

} // namespace

TEST_CASE("AC2: write_png applies exposure, PBR Neutral and the sRGB curve, within 1 per channel",
          "[core][image_io][accuracy]") {
    // Levels as fractions of the luminance that saturates the sensor at the exposure, 1.2 * 2^EV100
    // nits (colour.hpp): from black through the dark and mid tones, the knee of PBR Neutral at
    // 0.76, saturation at 1, and highlights far above it that roll off to white.
    constexpr std::array<double, 18> fractions = {0.0,  1e-4, 1e-3, 0.01, 0.03, 0.08,
                                                  0.18, 0.4,  0.7,  0.76, 0.8,  0.95,
                                                  1.0,  1.3,  2.0,  5.0,  20.0, 1000.0};
    const double ev100 = GENERATE(-2.0, 0.0, 8.0, 11.5);
    CAPTURE(ev100);
    const double saturation_nits = cs::ev100_saturation_factor * std::exp2(ev100);

    const int width = static_cast<int>(fractions.size());
    cs::Image image(width, 4);
    for (int x = 0; x < width; ++x) {
        const auto level =
            static_cast<float>(fractions[static_cast<std::size_t>(x)] * saturation_nits);
        image.at(x, 0) = {level, level, level};                // grey
        image.at(x, 1) = {level, 0.5f * level, 0.1f * level};  // warm
        image.at(x, 2) = {0.05f * level, level, 0.3f * level}; // green
        cs::Rng rng(cs::hash_seed(54, static_cast<std::uint32_t>(x), 3, 0, 0));
        const auto random_level = [&] {
            // Up to 4 times saturation, more often dark than bright.
            const double u = rng.next_double();
            return static_cast<float>(4.0 * u * u * u * saturation_nits);
        };
        const float r = random_level();
        const float g = random_level();
        const float b = random_level();
        image.at(x, 3) = {r, g, b};
    }
    // Negative and NaN channels: pbr_neutral takes them as 0.
    const auto sat = static_cast<float>(saturation_nits);
    image.at(0, 3) = {-1.0f * sat, 0.5f * sat, 0.2f * sat};
    image.at(1, 3) = {std::numeric_limits<float>::quiet_NaN(), 0.3f * sat, 0.1f * sat};

    const TempFile file("pipeline.png");
    cs::write_png(image, file.path(), ev100);

    const std::string path = file.path().string();
    CHECK_FALSE(stbi_is_16_bit(path.c_str()));
    int read_width = 0;
    int read_height = 0;
    int channels = 0;
    const std::unique_ptr<unsigned char, StbiFree> data(
        stbi_load(path.c_str(), &read_width, &read_height, &channels, 0));
    INFO((data ? "" : stbi_failure_reason()));
    REQUIRE(data != nullptr);
    REQUIRE(read_width == width);
    REQUIRE(read_height == 4);
    REQUIRE(channels == 3);

    int failures = 0;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < 3; ++c) {
                const int stored = data.get()[(y * width + x) * 3 + c];
                const int expected = expected_srgb8(image.at(x, y), ev100, c);
                if (std::abs(stored - expected) > 1) {
                    if (failures == 0) {
                        UNSCOPED_INFO("pixel (" << x << ", " << y << ") channel " << c
                                                << ": stored " << stored << ", expected "
                                                << expected);
                    }
                    ++failures;
                }
            }
        }
    }
    CHECK(failures == 0);
}

TEST_CASE("AC2: write_png maps black to 0 and far highlights to white", "[core][image_io]") {
    // Two values that need no rounding argument: 0 nits is 0 at any exposure, and 1e6 times
    // saturation tone maps to within 1e-6 of 1, which is 255.
    constexpr double ev100 = 8.0;
    const auto far = static_cast<float>(1e6 * cs::ev100_saturation_factor * std::exp2(ev100));
    cs::Image image(2, 1);
    image.at(0, 0) = {0.0f, 0.0f, 0.0f};
    image.at(1, 0) = {far, far, far};
    const TempFile file("black_white.png");
    cs::write_png(image, file.path(), ev100);

    int w = 0;
    int h = 0;
    int n = 0;
    const std::unique_ptr<unsigned char, StbiFree> data(
        stbi_load(file.path().string().c_str(), &w, &h, &n, 0));
    REQUIRE(data != nullptr);
    REQUIRE(w == 2);
    REQUIRE(h == 1);
    REQUIRE(n == 3);
    for (int c = 0; c < 3; ++c) {
        CHECK(data.get()[c] == 0);
        CHECK(data.get()[3 + c] == 255);
    }
}

TEST_CASE("write_png throws std::runtime_error when the file cannot be written",
          "[core][image_io]") {
    const cs::Image image(2, 2);
    CHECK_THROWS_AS(cs::write_png(image, missing_directory() / "image.png", 8.0),
                    std::runtime_error);
}

TEST_CASE("write_png refuses an exposure that is not finite", "[core][image_io]") {
    const cs::Image image(2, 2);
    const TempFile file("cs_core_image_io_png_nan_exposure.png");
    CHECK_THROWS_AS(cs::write_png(image, file.path(), std::numeric_limits<double>::quiet_NaN()),
                    std::invalid_argument);
    CHECK_THROWS_AS(cs::write_png(image, file.path(), std::numeric_limits<double>::infinity()),
                    std::invalid_argument);
}

TEST_CASE("write_exr refuses an image larger than read_exr reads", "[core][image_io]") {
    // One pixel too wide: never written, so a golden reference can always be read back.
    const cs::Image image(cs::max_image_side + 1, 1);
    const TempFile file("cs_core_image_io_too_wide.exr");
    CHECK_THROWS_AS(cs::write_exr(image, file.path()), std::runtime_error);
}

// --- Hostile EXR input ---------------------------------------------------------------------------

TEST_CASE("read_exr_from_memory refuses an empty span with std::runtime_error",
          "[core][image_io]") {
    CHECK_THROWS_AS(cs::read_exr_from_memory(std::span<const std::byte>{}), std::runtime_error);
}

TEST_CASE("read_exr_from_memory refuses random bytes with std::runtime_error", "[core][image_io]") {
    const std::uint32_t seed = GENERATE(1u, 2u, 3u);
    const bool with_magic = GENERATE(false, true);
    CAPTURE(seed, with_magic);
    cs::Rng rng(cs::hash_seed(55, seed, 0, 0, 0));
    std::vector<std::byte> bytes(4096);
    for (std::byte& b : bytes) {
        b = static_cast<std::byte>(rng.next_u32() & 0xffu);
    }
    if (with_magic) {
        // The EXR magic number and version 2, so the reader gets past the first check.
        const std::array<std::uint8_t, 8> start = {0x76, 0x2f, 0x31, 0x01, 2, 0, 0, 0};
        for (std::size_t i = 0; i < start.size(); ++i) {
            bytes[i] = static_cast<std::byte>(start[i]);
        }
    }
    CHECK_THROWS_AS(cs::read_exr_from_memory(bytes), std::runtime_error);
}

TEST_CASE("read_exr_from_memory refuses a truncated EXR file with std::runtime_error",
          "[core][image_io]") {
    cs::Image image(16, 16);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            cs::Rng rng(cs::hash_seed(56, static_cast<std::uint32_t>(x),
                                      static_cast<std::uint32_t>(y), 0, 0));
            image.at(x, y) = {rng.next_float(), rng.next_float(), rng.next_float()};
        }
    }
    const cs::ExrChannels channels = GENERATE(cs::ExrChannels::float32, cs::ExrChannels::half);
    CAPTURE(channels == cs::ExrChannels::half);
    const TempFile file(channels == cs::ExrChannels::half ? "truncated_half.exr"
                                                          : "truncated_float.exr");
    cs::write_exr(image, file.path(), channels);
    const std::vector<std::byte> whole = read_bytes(file.path());
    REQUIRE(whole.size() > 16);

    // The whole file reads, so what fails below is the truncation.
    CHECK_NOTHROW(cs::read_exr_from_memory(whole));

    const std::span<const std::byte> bytes(whole);
    SECTION("cut in half") {
        CHECK_THROWS_AS(cs::read_exr_from_memory(bytes.first(whole.size() / 2)),
                        std::runtime_error);
    }
    SECTION("only the magic number") {
        CHECK_THROWS_AS(cs::read_exr_from_memory(bytes.first(4)), std::runtime_error);
    }
    SECTION("only the magic number and version") {
        CHECK_THROWS_AS(cs::read_exr_from_memory(bytes.first(8)), std::runtime_error);
    }
    SECTION("the last byte missing") {
        CHECK_THROWS_AS(cs::read_exr_from_memory(bytes.first(whole.size() - 1)),
                        std::runtime_error);
    }
}

TEST_CASE("read_exr refuses a file that does not exist, or a directory, with std::runtime_error",
          "[core][image_io]") {
    const std::filesystem::path missing =
        std::filesystem::temp_directory_path() / "cs_core_test_image_io_does_not_exist.exr";
    REQUIRE_FALSE(std::filesystem::exists(missing));
    CHECK_THROWS_AS(cs::read_exr(missing), std::runtime_error);
    CHECK_THROWS_AS(cs::read_exr(std::filesystem::temp_directory_path()), std::runtime_error);
}

TEST_CASE("read_exr_from_memory reads a handmade uncompressed float EXR", "[core][image_io]") {
    // The positive control for the handmade files below: with every field valid, the file reads,
    // so a refusal below is due to the one field each test changes.
    check_2x2(cs::read_exr_from_memory(handmade_exr({0, 0, 1, 1}, bgr, bgr_2x2)));
}

TEST_CASE("read_exr ignores channels other than R, G and B, such as alpha", "[core][image_io]") {
    const std::vector<std::string> abgr = {"A", "B", "G", "R"};
    std::vector<std::vector<float>> pixels = {{0.5f, 0.5f, 0.5f, 0.5f}};
    pixels.insert(pixels.end(), bgr_2x2.begin(), bgr_2x2.end());
    check_2x2(cs::read_exr_from_memory(handmade_exr({0, 0, 1, 1}, abgr, pixels)));
}

TEST_CASE("read_exr refuses an EXR file without R, G and B channels", "[core][image_io]") {
    SECTION("no R") {
        const std::vector<std::vector<float>> bg = {bgr_2x2[0], bgr_2x2[1]};
        CHECK_THROWS_AS(cs::read_exr_from_memory(handmade_exr({0, 0, 1, 1}, {"B", "G"}, bg)),
                        std::runtime_error);
    }
    SECTION("luminance only") {
        CHECK_THROWS_AS(cs::read_exr_from_memory(handmade_exr({0, 0, 1, 1}, {"Y"}, {bgr_2x2[2]})),
                        std::runtime_error);
    }
}

TEST_CASE("read_exr refuses a header whose size exceeds max_image_side or max_image_pixels",
          "[core][image_io]") {
    // Only the header: a reader that checks the size first, as image_io.hpp promises, refuses it
    // without allocating for the pixels. 8193 x 8193 has sides within max_image_side, but more
    // than max_image_pixels = 8192 x 8192 pixels.
    static_assert(cs::max_image_side == 16384);
    static_assert(std::int64_t{8193} * 8193 > cs::max_image_pixels);
    const Box2i window = GENERATE(Box2i{0, 0, cs::max_image_side, 0},  // width 16385
                                  Box2i{0, 0, 0, cs::max_image_side},  // height 16385
                                  Box2i{0, 0, 8192, 8192},             // 8193 x 8193
                                  Box2i{0, 0, 1'000'000, 1'000'000},   // far too big
                                  Box2i{-(1 << 30), 0, (1 << 30), 0}); // width 2^31 + 1
    CAPTURE(window.xmin, window.ymin, window.xmax, window.ymax);
    CHECK_THROWS_AS(cs::read_exr_from_memory(handmade_exr(window, bgr, {})), std::runtime_error);
}

TEST_CASE("read_exr refuses a header whose data window is empty or overflows int",
          "[core][image_io]") {
    const Box2i window = GENERATE(Box2i{0, 0, -1, 1}, // width 0
                                  Box2i{0, 0, 1, -1}, // height 0
                                  Box2i{5, 5, 0, 0},  // inverted
                                  Box2i{std::numeric_limits<std::int32_t>::min(), 0,
                                        std::numeric_limits<std::int32_t>::max(), 0});
    CAPTURE(window.xmin, window.ymin, window.xmax, window.ymax);
    CHECK_THROWS_AS(cs::read_exr_from_memory(handmade_exr(window, bgr, {})), std::runtime_error);
}

// --- Display PNG (M1-18) -------------------------------------------------------------------------
//
// write_display_png writes an image that is already display-referred linear RGB, such as the FLIP
// heat map the golden-image runner writes as flip.png on a failure (M1-18, acceptance criterion
// 2): every channel through linear_to_srgb8 alone, with no exposure and no tone mapping.

namespace {

// An 8-bit PNG read back with stb_image.
struct Png {
    int width = 0;
    int height = 0;
    int channels = 0;
    bool sixteen_bit = false;
    std::vector<unsigned char> bytes;

    int at(int x, int y, int c) const {
        return bytes[static_cast<std::size_t>((y * width + x) * channels + c)];
    }
};

Png read_png(const std::filesystem::path& path) {
    const std::string name = path.string();
    Png png;
    png.sixteen_bit = stbi_is_16_bit(name.c_str()) != 0;
    const std::unique_ptr<unsigned char, StbiFree> data(
        stbi_load(name.c_str(), &png.width, &png.height, &png.channels, 0));
    INFO((data ? "" : stbi_failure_reason()));
    REQUIRE(data != nullptr);
    png.bytes.assign(data.get(), data.get() + static_cast<std::size_t>(png.width) * png.height *
                                                  static_cast<std::size_t>(png.channels));
    return png;
}

float channel_of(const cs::Rgb& rgb, int c) {
    return c == 0 ? rgb.r : (c == 1 ? rgb.g : rgb.b);
}

// The first channel the PNG does not store as linear_to_srgb8 of the image's, or "" if it stores
// every one so. The PNG must have the image's size and 3 channels.
std::string first_display_difference(const cs::Image& image, const Png& png) {
    REQUIRE(png.width == image.width());
    REQUIRE(png.height == image.height());
    REQUIRE(png.channels == 3);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            for (int c = 0; c < 3; ++c) {
                const float value = channel_of(image.at(x, y), c);
                const int expected = cs::linear_to_srgb8(value);
                const int stored = png.at(x, y, c);
                if (stored != expected) {
                    std::ostringstream out;
                    out.precision(9);
                    out << "pixel (" << x << ", " << y << ") channel " << c << ": " << value
                        << " stored as " << stored << ", linear_to_srgb8 gives " << expected;
                    return out.str();
                }
            }
        }
    }
    return "";
}

// The sRGB decoding curve of IEC 61966-2-1, in double.
double srgb_to_linear(double encoded) {
    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
}

} // namespace

TEST_CASE("M1-18 AC2: write_display_png stores every channel as linear_to_srgb8 of it, exactly",
          "[core][image_io]") {
    constexpr int width = 64;
    constexpr int height = 6;
    cs::Image image(width, height);
    for (int x = 0; x < width; ++x) {
        const float t = static_cast<float>(x) / 63.0f;
        // A grey ramp over [0, 1].
        image.at(x, 0) = {t, t, t};
        // Three different ramps, so a swapped channel or a transposed image shows.
        image.at(x, 1) = {t, 1.0f - t, static_cast<float>((x * 37) % 64) / 63.0f};
        cs::Rng rng(cs::hash_seed(90, static_cast<std::uint32_t>(x), 0, 0, 0));
        // Random values in [0, 1).
        const float r = rng.next_float();
        const float g = rng.next_float();
        const float b = rng.next_float();
        image.at(x, 2) = {r, g, b};
        // Random dark values, many below 0.0031308, where the sRGB curve is linear.
        const float u = rng.next_float();
        const float v = rng.next_float();
        const float w = rng.next_float();
        image.at(x, 3) = {u * u * u * u, v * v * v * v, w * w * w * w};
        // The edges of [0, 1] and just inside them.
        image.at(x, 4) = {0.0f, 1.0f, (x % 2 == 0) ? 1e-6f : 0.999999f};
        // Different values in every pixel of the last row, for a flipped image.
        image.at(x, 5) = {1.0f - t, t * t, 0.5f * t};
    }

    const TempFile file("display_exact.png");
    cs::write_display_png(image, file.path());
    const Png png = read_png(file.path());
    CHECK_FALSE(png.sixteen_bit);
    const std::string difference = first_display_difference(image, png);
    INFO(difference);
    CHECK(difference.empty());
}

TEST_CASE("M1-18 AC2: write_display_png applies no exposure or tone mapping: mid grey 0.2 is 124, "
          "(0.9, 0.05, 0.02) is (243, 63, 39)",
          "[core][image_io]") {
    // The sRGB curve alone, 255 (1.055 v^(1/2.4) - 0.055) rounded to the nearest integer:
    //   0.2  -> 123.555 -> 124
    //   0.9  -> 243.445 -> 243
    //   0.05 ->  63.189 ->  63
    //   0.02 ->  38.684 ->  39
    // each at least 0.05 from a rounding boundary, far more than float arithmetic moves it.
    // write_png's pipeline would change both: even at the exposure that leaves the values as they
    // are (scale 1, EV100 = log2(1 / 1.2)), PBR Neutral lowers 0.2 by its offset 0.04 to 0.16,
    // stored as 111, and compresses and desaturates the saturated colour to (0.841, 0.036, 0.0076),
    // stored as (236, 53, 21).
    cs::Image image(2, 1);
    image.at(0, 0) = {0.2f, 0.2f, 0.2f};
    image.at(1, 0) = {0.9f, 0.05f, 0.02f};

    const TempFile file("display_no_tone_mapping.png");
    cs::write_display_png(image, file.path());
    const Png png = read_png(file.path());
    REQUIRE(png.width == 2);
    REQUIRE(png.height == 1);
    REQUIRE(png.channels == 3);
    CHECK(png.at(0, 0, 0) == 124);
    CHECK(png.at(0, 0, 1) == 124);
    CHECK(png.at(0, 0, 2) == 124);
    CHECK(png.at(1, 0, 0) == 243);
    CHECK(png.at(1, 0, 1) == 63);
    CHECK(png.at(1, 0, 2) == 39);
    // And the same as linear_to_srgb8, which the header names.
    const std::string difference = first_display_difference(image, png);
    INFO(difference);
    CHECK(difference.empty());
}

TEST_CASE("M1-18 AC2: write_display_png stores values above 1 as 255, and negative and NaN values "
          "as 0",
          "[core][image_io]") {
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr float inf = std::numeric_limits<float>::infinity();
    constexpr float largest = std::numeric_limits<float>::max();
    cs::Image image(4, 2);
    // Above 1, up to infinity; 1 itself is 255 too.
    image.at(0, 0) = {1.0000001f, 1.5f, 2.0f};
    image.at(1, 0) = {1e30f, largest, inf};
    image.at(2, 0) = {65504.0f, 255.0f, 1.0f};
    image.at(3, 0) = {inf, 1.0f, 3.0f};
    // Negative, down to minus infinity, and NaN; 0 and negative zero are 0 too.
    image.at(0, 1) = {-0.0f, -1e-30f, -0.5f};
    image.at(1, 1) = {-1e30f, -inf, nan};
    image.at(2, 1) = {nan, nan, nan};
    image.at(3, 1) = {-1.0f, 0.0f, -largest};

    const TempFile file("display_clamp.png");
    cs::write_display_png(image, file.path());
    const Png png = read_png(file.path());
    REQUIRE(png.width == 4);
    REQUIRE(png.height == 2);
    REQUIRE(png.channels == 3);
    for (int x = 0; x < 4; ++x) {
        for (int c = 0; c < 3; ++c) {
            CAPTURE(x, c);
            CHECK(png.at(x, 0, c) == 255);
            CHECK(png.at(x, 1, c) == 0);
        }
    }
}

TEST_CASE("M1-18 AC2: write_display_png writes an 8-bit PNG with 3 channels and the image's size",
          "[core][image_io]") {
    // 128 x 72 is the size of the skeleton golden image; the others catch a transposed width and
    // height, and the smallest image.
    const std::pair<int, int> size =
        GENERATE(values<std::pair<int, int>>({{1, 1}, {3, 2}, {2, 3}, {37, 23}, {128, 72}}));
    const int width = size.first;
    const int height = size.second;
    CAPTURE(width, height);
    cs::Image image(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // Different in every pixel and channel, so the positions are checked too.
            image.at(x, y) = {static_cast<float>(x) / static_cast<float>(width),
                              static_cast<float>(y) / static_cast<float>(height),
                              static_cast<float>(x + y) / static_cast<float>(width + height)};
        }
    }

    const TempFile file("display_size.png");
    cs::write_display_png(image, file.path());
    const Png png = read_png(file.path());
    CHECK(png.width == width);
    CHECK(png.height == height);
    CHECK(png.channels == 3);
    CHECK_FALSE(png.sixteen_bit);
    const std::string difference = first_display_difference(image, png);
    INFO(difference);
    CHECK(difference.empty());
}

TEST_CASE("M1-18 AC2: magma's colours as linear RGB are written as magma's 8-bit sRGB colours",
          "[core][image_io]") {
    // The heat map holds FLIP's magma map, defined in sRGB, as linear RGB (image_compare.hpp);
    // written with write_display_png it must show the map's own colours: 255 c rounded, for each
    // sRGB channel c of each entry. The channel closest to a rounding boundary, entry 160's red
    // (255 * 0.31961 = 81.50055), is 5.5e-4 from it; the round trip through float linear RGB moves
    // 255 c by about 1e-5. Allowing 0.51 accepts either neighbour there and nothing else.
    cs::Image image(256, 1);
    for (int i = 0; i < 256; ++i) {
        // color3's constructor sets x, y and z, which are its r, g and b.
        const FLIP::color3& srgb = FLIP::MapMagma[i];
        image.at(i, 0) = {static_cast<float>(srgb_to_linear(srgb.x)),
                          static_cast<float>(srgb_to_linear(srgb.y)),
                          static_cast<float>(srgb_to_linear(srgb.z))};
    }

    const TempFile file("display_magma.png");
    cs::write_display_png(image, file.path());
    const Png png = read_png(file.path());
    REQUIRE(png.width == 256);
    REQUIRE(png.height == 1);
    REQUIRE(png.channels == 3);

    int failures = 0;
    for (int i = 0; i < 256; ++i) {
        const FLIP::color3& srgb = FLIP::MapMagma[i];
        const std::array<double, 3> expected = {255.0 * srgb.x, 255.0 * srgb.y, 255.0 * srgb.z};
        for (int c = 0; c < 3; ++c) {
            const int stored = png.at(i, 0, c);
            if (!(std::abs(stored - expected[static_cast<std::size_t>(c)]) <= 0.51)) {
                if (failures == 0) {
                    UNSCOPED_INFO("magma entry " << i << " channel " << c << ": stored " << stored
                                                 << ", expected "
                                                 << expected[static_cast<std::size_t>(c)]);
                }
                ++failures;
            }
        }
    }
    CHECK(failures == 0);
}

TEST_CASE("M1-18: write_display_png throws std::runtime_error when the file cannot be written",
          "[core][image_io]") {
    const cs::Image image(2, 2);
    SECTION("in a directory that does not exist") {
        CHECK_THROWS_AS(cs::write_display_png(image, missing_directory() / "image.png"),
                        std::runtime_error);
    }
    SECTION("at a path that is a directory") {
        CHECK_THROWS_AS(cs::write_display_png(image, std::filesystem::temp_directory_path()),
                        std::runtime_error);
    }
    SECTION("at an empty path") {
        CHECK_THROWS_AS(cs::write_display_png(image, std::filesystem::path()), std::runtime_error);
    }
}
