// SPDX-License-Identifier: Apache-2.0

// Slang shaders compiled and embedded at build time (pipeline.hpp), and the first pixels: a
// triangle rendered into an offscreen target and read back into a cs::Image (offscreen.hpp),
// issue M1-20.
//
// The shader fixture is libs/gpu/shaders/triangle.slang: no vertex input, three vertices by
// SV_VulkanVertexID at (-0.5, -0.5), (0.5, -0.5) and (0, 0.5) in clip space (y points down), at
// depth 0.5, in the flat linear RGBA colour of a 16-byte push constant. It covers the centre of the
// target and none of its corners.
//
// Every test that needs a device starts with make_device() (gpu_test.hpp): without a Vulkan
// device it skips, and with CS_REQUIRE_GPU=1 it fails. The harness (main.cpp) fails the running
// test on any validation warning or error, so every test here is also the "validation clean"
// check of criterion 3. Vulkan objects are declared after the device, so they are destroyed
// before it, also when an assertion throws.
//
// The "a syntax error fails the build" half of criterion 1 is a CTest in libs/gpu/CMakeLists.txt,
// not a Catch2 test.

#include "gpu_test.hpp"

#include <cs/core/image.hpp>
#include <cs/gpu/device.hpp>
#include <cs/gpu/offscreen.hpp>
#include <cs/gpu/pipeline.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::Message;
using Catch::Matchers::MessageMatches;
using cs::gpu::ClearValues;
using cs::gpu::Device;
using cs::gpu::GraphicsPipeline;
using cs::gpu::GraphicsPipelineOptions;
using cs::gpu::OffscreenTarget;
using cs::gpu::test::make_device;

namespace {

// The push constant of triangle.slang: float4 colour, linear RGBA.
using Colour = std::array<float, 4>;
constexpr Colour red{1.0f, 0.0f, 0.0f, 1.0f};
constexpr Colour green{0.0f, 1.0f, 0.0f, 1.0f};
constexpr std::uint32_t colour_bytes = sizeof(Colour); // 16

// SPIR-V 1.6 (the version word is 0x00MMmm00), what pipeline.hpp promises.
constexpr std::uint32_t spirv_1_6 = 0x00010600;
// A SPIR-V header is five words: magic, version, generator, bound, schema.
constexpr std::size_t spirv_header_words = 5;

VkPhysicalDeviceLimits limits_of(const Device& device) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.physical_device(), &properties);
    return properties.limits;
}

GraphicsPipelineOptions triangle_options(const OffscreenTarget& target) {
    GraphicsPipelineOptions options;
    options.shader = "triangle";
    options.colour_format = OffscreenTarget::colour_format;
    options.depth_format = target.depth_format();
    options.push_constant_bytes = colour_bytes;
    return options;
}

// Records the triangle in a colour: bind, push, draw three vertices.
OffscreenTarget::DrawFunction draw_triangle(const GraphicsPipeline& pipeline, Colour colour) {
    return [&pipeline, colour](VkCommandBuffer commands) {
        pipeline.bind(commands);
        pipeline.push_constants(commands, colour);
        vkCmdDraw(commands, 3, 1, 0, 0);
    };
}

void draw_nothing(VkCommandBuffer /*commands*/) {}

ClearValues clear_to(float r, float g, float b, float a = 1.0f, float depth = 1.0f) {
    ClearValues clear;
    clear.colour = {r, g, b, a};
    clear.depth = depth;
    return clear;
}

std::string describe(const cs::Rgb& pixel) {
    std::ostringstream text;
    text << '(' << pixel.r << ", " << pixel.g << ", " << pixel.b << ')';
    return text.str();
}

// Exact equality on purpose: see the tests for why each comparison may be exact.
bool same(const cs::Rgb& pixel, float r, float g, float b) {
    return pixel.r == r && pixel.g == g && pixel.b == b;
}

bool same(const cs::Rgb& a, const cs::Rgb& b) {
    return same(a, b.r, b.g, b.b);
}

// Every pixel of a pixel-for-pixel comparison, with the first difference for the log.
bool identical(const cs::Image& a, const cs::Image& b, std::string& first_difference) {
    if (a.width() != b.width() || a.height() != b.height()) {
        first_difference = "sizes differ";
        return false;
    }
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (!same(a.at(x, y), b.at(x, y))) {
                first_difference = "pixel (" + std::to_string(x) + ", " + std::to_string(y) +
                                   "): " + describe(a.at(x, y)) + " vs " + describe(b.at(x, y));
                return false;
            }
        }
    }
    return true;
}

} // namespace

// ---- Criterion 1: every libs/gpu/shaders/*.slang is compiled (found by glob) and embedded -------

// Needs no device.
TEST_CASE("offscreen: every libs/gpu/shaders/*.slang is embedded as SPIR-V 1.6 under its stem",
          "[gpu]") {
    const std::filesystem::path shader_dir =
        std::filesystem::path(CS_SOURCE_DIR) / "libs" / "gpu" / "shaders";
    REQUIRE(std::filesystem::is_directory(shader_dir));

    std::vector<std::string> expected;
    for (const auto& entry : std::filesystem::directory_iterator(shader_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".slang") {
            expected.push_back(entry.path().stem().string());
        }
    }
    std::sort(expected.begin(), expected.end());
    REQUIRE_FALSE(expected.empty()); // triangle.slang at least
    REQUIRE(std::find(expected.begin(), expected.end(), "triangle") != expected.end());

    for (const std::string& name : expected) {
        INFO("shader " << name);
        const std::span<const std::uint32_t> spirv = cs::gpu::shader_spirv(name);
        REQUIRE(spirv.size() > spirv_header_words); // a header and at least one instruction
        CHECK(spirv[0] == cs::gpu::spirv_magic);
        CHECK(spirv[1] == spirv_1_6);
        CHECK(spirv[3] > 0);  // the id bound: every module defines ids
        CHECK(spirv[4] == 0); // the schema word is reserved, 0
    }

    SECTION("embedded_shaders() lists exactly the .slang files' stems, in name order") {
        const std::span<const cs::gpu::EmbeddedShader> shaders = cs::gpu::embedded_shaders();
        std::vector<std::string> names;
        for (const cs::gpu::EmbeddedShader& shader : shaders) {
            names.emplace_back(shader.name);
        }
        CHECK(names == expected); // same names, same order, no extras
        CHECK(std::is_sorted(names.begin(), names.end()));

        for (const cs::gpu::EmbeddedShader& shader : shaders) {
            INFO("shader " << shader.name);
            REQUIRE(shader.spirv.size() > spirv_header_words);
            CHECK(shader.spirv[0] == cs::gpu::spirv_magic);
            const std::span<const std::uint32_t> by_name = cs::gpu::shader_spirv(shader.name);
            CHECK(std::equal(shader.spirv.begin(), shader.spirv.end(), by_name.begin(),
                             by_name.end()));
        }
    }
}

TEST_CASE("offscreen: shader_spirv throws std::out_of_range, naming it, for a shader that is not "
          "embedded",
          "[gpu]") {
    CHECK_THROWS_MATCHES(cs::gpu::shader_spirv("no such shader"), std::out_of_range,
                         MessageMatches(ContainsSubstring("no such shader")));
    CHECK_THROWS_AS(cs::gpu::shader_spirv(""), std::out_of_range);
    // Shaders are named by stem, not by file name.
    CHECK_THROWS_MATCHES(cs::gpu::shader_spirv("triangle.slang"), std::out_of_range,
                         MessageMatches(ContainsSubstring("triangle.slang")));
}

// ---- Criterion 2: an offscreen target (RGBA16F colour and depth) reads back to cs::Image --------

TEST_CASE("offscreen: a target has RGBA16F colour, a D32 or X8_D24 depth image, and their views",
          "[gpu]") {
    const std::unique_ptr<Device> gpu = make_device();
    const OffscreenTarget target(*gpu, 64, 64);

    STATIC_CHECK(OffscreenTarget::colour_format == VK_FORMAT_R16G16B16A16_SFLOAT);
    CHECK(target.width() == 64);
    CHECK(target.height() == 64);
    const VkFormat depth = target.depth_format();
    CHECK((depth == VK_FORMAT_D32_SFLOAT || depth == VK_FORMAT_X8_D24_UNORM_PACK32));
    CHECK(target.colour_image() != VK_NULL_HANDLE);
    CHECK(target.colour_view() != VK_NULL_HANDLE);
    CHECK(target.depth_image() != VK_NULL_HANDLE);
    CHECK(target.depth_view() != VK_NULL_HANDLE);
}

TEST_CASE("offscreen: an empty frame reads back as the clear colour, exactly, in every pixel",
          "[gpu]") {
    const std::unique_ptr<Device> gpu = make_device();
    OffscreenTarget target(*gpu, 64, 64);

    // 0.25 = 2^-2, 0.5 = 2^-1 and 2.0 = 2^1 are exact in half float (and in float), so the clear
    // (float -> half) and the readback (half -> float) are both exact and == is the right
    // comparison. 2.0 also checks that light above 1 is kept, not clamped. Alpha is dropped.
    const cs::Image image = target.render(draw_nothing, clear_to(0.25f, 0.5f, 2.0f, 0.75f));

    REQUIRE(image.width() == 64);
    REQUIRE(image.height() == 64);
    int wrong = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (!same(image.at(x, y), 0.25f, 0.5f, 2.0f)) {
                if (wrong == 0) {
                    UNSCOPED_INFO("first wrong pixel (" << x << ", " << y
                                                        << "): " << describe(image.at(x, y)));
                }
                ++wrong;
            }
        }
    }
    CHECK(wrong == 0);
}

TEST_CASE("offscreen: a non-square target reads back width by height, and a 1x1 target works",
          "[gpu]") {
    const std::unique_ptr<Device> gpu = make_device();

    SECTION("7 by 3") {
        OffscreenTarget target(*gpu, 7, 3);
        CHECK(target.width() == 7);
        CHECK(target.height() == 3);
        const cs::Image image = target.render(draw_nothing, clear_to(0.5f, 0.25f, 0.0f));
        REQUIRE(image.width() == 7);
        REQUIRE(image.height() == 3);
        for (int y = 0; y < 3; ++y) {
            for (int x = 0; x < 7; ++x) {
                INFO("pixel (" << x << ", " << y << "): " << describe(image.at(x, y)));
                CHECK(same(image.at(x, y), 0.5f, 0.25f, 0.0f));
            }
        }
    }
    SECTION("1 by 1") {
        OffscreenTarget target(*gpu, 1, 1);
        const cs::Image image = target.render(draw_nothing, clear_to(2.0f, 0.5f, 0.25f));
        REQUIRE(image.width() == 1);
        REQUIRE(image.height() == 1);
        INFO(describe(image.at(0, 0)));
        CHECK(same(image.at(0, 0), 2.0f, 0.5f, 0.25f));
    }
}

TEST_CASE("offscreen: the target rejects a zero size, and one above maxImageDimension2D", "[gpu]") {
    const std::unique_ptr<Device> gpu = make_device();
    const std::uint32_t max_size = limits_of(*gpu).maxImageDimension2D;
    REQUIRE(max_size >= 4096); // Vulkan's required minimum

    CHECK_THROWS_AS(OffscreenTarget(*gpu, 0, 64), std::invalid_argument);
    CHECK_THROWS_AS(OffscreenTarget(*gpu, 64, 0), std::invalid_argument);
    CHECK_THROWS_AS(OffscreenTarget(*gpu, 0, 0), std::invalid_argument);
    CHECK_THROWS_AS(OffscreenTarget(*gpu, max_size + 1, 64), std::invalid_argument);
    CHECK_THROWS_AS(OffscreenTarget(*gpu, 64, max_size + 1), std::invalid_argument);
    CHECK_THROWS_AS(OffscreenTarget(*gpu, UINT32_MAX, UINT32_MAX), std::invalid_argument);
}

// ---- Criterion 3: a 64x64 red triangle, centre exactly (1, 0, 0), corners (0, 0, 0) ------------

// Why == is right here: the fragment shader writes the push-constant colour unchanged; 1.0 and
// 0.0 are exact in half float, so the store into RGBA16F and the readback into float are exact;
// there is no blending, no MSAA (one sample, so no coverage averaging at edges) and no filtering.
// Every pixel is therefore either exactly the triangle's colour or exactly the clear colour.
TEST_CASE("offscreen: a 64x64 red triangle has its centre exactly (1, 0, 0) and its corners "
          "(0, 0, 0)",
          "[gpu]") {
    const std::unique_ptr<Device> gpu = make_device();
    OffscreenTarget target(*gpu, 64, 64);
    const GraphicsPipeline pipeline(*gpu, triangle_options(target));
    CHECK(pipeline.pipeline() != VK_NULL_HANDLE);
    CHECK(pipeline.layout() != VK_NULL_HANDLE);
    CHECK(pipeline.push_constant_bytes() == colour_bytes);

    const cs::Image image = target.render(draw_triangle(pipeline, red), clear_to(0, 0, 0, 0));
    REQUIRE(image.width() == 64);
    REQUIRE(image.height() == 64);

    INFO("centre " << describe(image.at(32, 32)));
    CHECK(same(image.at(32, 32), 1.0f, 0.0f, 0.0f));
    for (const auto& [x, y] :
         std::array<std::array<int, 2>, 4>{{{0, 0}, {63, 0}, {0, 63}, {63, 63}}}) {
        INFO("corner (" << x << ", " << y << "): " << describe(image.at(x, y)));
        CHECK(same(image.at(x, y), 0.0f, 0.0f, 0.0f));
    }

    SECTION("every pixel is exactly red or exactly black, and the red area is the triangle's") {
        // The triangle covers 1/8 of clip space ([-1, 1]^2 has area 4, the triangle 0.5), so
        // 4096 / 8 = 512 pixel centres in theory. Each of its 32 rows of pixels can gain or lose
        // at most one pixel to where the edges fall between centres, hence the tolerance of 32.
        int reds = 0;
        int others = 0;
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const cs::Rgb& p = image.at(x, y);
                if (same(p, 1.0f, 0.0f, 0.0f)) {
                    ++reds;
                } else if (!same(p, 0.0f, 0.0f, 0.0f)) {
                    if (others == 0) {
                        UNSCOPED_INFO("first pixel neither red nor black ("
                                      << x << ", " << y << "): " << describe(p));
                    }
                    ++others;
                }
            }
        }
        CHECK(others == 0);
        CHECK(reds >= 512 - 32);
        CHECK(reds <= 512 + 32);
    }

    SECTION("row 0 is the top: the triangle's wide edge is near the top, its point at the bottom") {
        // Pixel (20, 17) has its centre at clip (-0.359, -0.453); the triangle's half-width
        // there is 0.5 * (0.5 - y) = 0.477, so it is inside, 3.8 pixels from the slanted edge
        // and 1.5 pixels below the top edge (row 16.0). Pixel (20, 46) is its mirror image at
        // clip y = +0.453, where the half-width is 0.023: outside. An image read back upside
        // down would swap them.
        INFO("(20, 17) " << describe(image.at(20, 17)) << ", (20, 46) "
                         << describe(image.at(20, 46)));
        CHECK(same(image.at(20, 17), 1.0f, 0.0f, 0.0f));
        CHECK(same(image.at(20, 46), 0.0f, 0.0f, 0.0f));
        // Above the top edge and below the bottom point: clip y = -0.70 and +0.64.
        CHECK(same(image.at(32, 9), 0.0f, 0.0f, 0.0f));
        CHECK(same(image.at(32, 52), 0.0f, 0.0f, 0.0f));
    }
}

TEST_CASE("offscreen: the default clear values are black at the far plane", "[gpu]") {
    const std::unique_ptr<Device> gpu = make_device();
    OffscreenTarget target(*gpu, 64, 64);
    const GraphicsPipeline pipeline(*gpu, triangle_options(target));

    const ClearValues defaults;
    CHECK(defaults.depth == 1.0f);
    CHECK(defaults.colour == std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});

    // render(draw) with the defaults: depth 1.0 lets the triangle (at 0.5) through.
    const cs::Image image = target.render(draw_triangle(pipeline, red));
    CHECK(same(image.at(32, 32), 1.0f, 0.0f, 0.0f));
    CHECK(same(image.at(0, 0), 0.0f, 0.0f, 0.0f));
}

// ---- Criterion 3, validation clean: a target renders again, and survives a failed draw ---------

TEST_CASE("offscreen: rendering again with the same target gives the same image", "[gpu]") {
    const std::unique_ptr<Device> gpu = make_device();
    OffscreenTarget target(*gpu, 64, 64);
    const GraphicsPipeline pipeline(*gpu, triangle_options(target));
    const ClearValues clear = clear_to(0.0f, 0.0f, 0.25f);

    // The second and third frames record barriers against the previous frame's copy to the
    // readback buffer; the harness fails the test on any hazard the validation layer sees.
    const cs::Image first = target.render(draw_triangle(pipeline, red), clear);
    const cs::Image second = target.render(draw_triangle(pipeline, red), clear);
    std::string difference;
    const bool same_frames = identical(first, second, difference);
    INFO(difference); // INFO captures its message here, so after the comparison
    CHECK(same_frames);

    SECTION("a different frame in between is not left behind") {
        const cs::Image other = target.render(draw_triangle(pipeline, green), clear);
        INFO("centre " << describe(other.at(32, 32)));
        CHECK(same(other.at(32, 32), 0.0f, 1.0f, 0.0f));
        CHECK(same(other.at(0, 0), 0.0f, 0.0f, 0.25f));

        const cs::Image again = target.render(draw_triangle(pipeline, red), clear);
        std::string again_difference;
        const bool same_again = identical(first, again, again_difference);
        INFO(again_difference);
        CHECK(same_again);
    }
}

TEST_CASE("offscreen: an exception from the draw function propagates, and the target still "
          "renders",
          "[gpu]") {
    const std::unique_ptr<Device> gpu = make_device();
    OffscreenTarget target(*gpu, 64, 64);
    const GraphicsPipeline pipeline(*gpu, triangle_options(target));

    SECTION("thrown before recording anything") {
        CHECK_THROWS_MATCHES(static_cast<void>(target.render([](VkCommandBuffer) {
                                 throw std::runtime_error("deliberate draw failure");
                             })),
                             std::runtime_error, Message("deliberate draw failure"));
    }
    SECTION("thrown after binding and pushing") {
        CHECK_THROWS_MATCHES(static_cast<void>(target.render([&pipeline](VkCommandBuffer c) {
                                 pipeline.bind(c);
                                 pipeline.push_constants(c, green);
                                 throw std::runtime_error("deliberate draw failure");
                             })),
                             std::runtime_error, Message("deliberate draw failure"));
    }
    SECTION("thrown twice in a row") {
        for (int i = 0; i < 2; ++i) {
            CHECK_THROWS_AS(static_cast<void>(target.render([](VkCommandBuffer) {
                                throw std::runtime_error("deliberate draw failure");
                            })),
                            std::runtime_error);
        }
    }

    const cs::Image image = target.render(draw_triangle(pipeline, red), clear_to(0, 0, 0));
    INFO("centre " << describe(image.at(32, 32)) << ", corner " << describe(image.at(0, 0)));
    CHECK(same(image.at(32, 32), 1.0f, 0.0f, 0.0f));
    CHECK(same(image.at(0, 0), 0.0f, 0.0f, 0.0f));
}

// ---- Header: GraphicsPipeline rejects what it cannot build -------------------------------------

TEST_CASE("offscreen: GraphicsPipeline rejects an unknown shader, a missing entry point, no colour "
          "format and a bad push-constant size",
          "[gpu]") {
    const std::unique_ptr<Device> gpu = make_device();
    const std::uint32_t max_push = limits_of(*gpu).maxPushConstantsSize;
    REQUIRE(max_push >= 128); // Vulkan's required minimum

    GraphicsPipelineOptions good;
    good.shader = "triangle";
    good.colour_format = OffscreenTarget::colour_format;
    good.depth_format = VK_FORMAT_UNDEFINED; // no depth attachment: valid, nothing to render here
    good.push_constant_bytes = colour_bytes;

    SECTION("the good options build, without depth") {
        const GraphicsPipeline pipeline(*gpu, good);
        CHECK(pipeline.pipeline() != VK_NULL_HANDLE);
        CHECK(pipeline.push_constant_bytes() == colour_bytes);
    }
    SECTION("the whole push-constant range of the device builds") {
        GraphicsPipelineOptions options = good;
        options.push_constant_bytes = max_push;
        const GraphicsPipeline pipeline(*gpu, options);
        CHECK(pipeline.push_constant_bytes() == max_push);
    }
    SECTION("an unknown shader: std::out_of_range, naming it") {
        GraphicsPipelineOptions options = good;
        options.shader = "no such shader";
        CHECK_THROWS_MATCHES(GraphicsPipeline(*gpu, options), std::out_of_range,
                             MessageMatches(ContainsSubstring("no such shader")));
        options.shader = "";
        CHECK_THROWS_AS(GraphicsPipeline(*gpu, options), std::out_of_range);
    }
    SECTION("a vertex entry point the shader lacks: std::invalid_argument, naming it") {
        GraphicsPipelineOptions options = good;
        options.vertex_entry = "no_such_entry";
        CHECK_THROWS_MATCHES(GraphicsPipeline(*gpu, options), std::invalid_argument,
                             MessageMatches(ContainsSubstring("no_such_entry")));
    }
    SECTION("a fragment entry point the shader lacks: std::invalid_argument, naming it") {
        GraphicsPipelineOptions options = good;
        options.fragment_entry = "no_such_entry";
        CHECK_THROWS_MATCHES(GraphicsPipeline(*gpu, options), std::invalid_argument,
                             MessageMatches(ContainsSubstring("no_such_entry")));
    }
    SECTION("an empty entry point: std::invalid_argument") {
        GraphicsPipelineOptions options = good;
        options.vertex_entry = "";
        CHECK_THROWS_AS(GraphicsPipeline(*gpu, options), std::invalid_argument);
    }
    SECTION("no colour format: std::invalid_argument") {
        GraphicsPipelineOptions options = good;
        options.colour_format = VK_FORMAT_UNDEFINED;
        CHECK_THROWS_AS(GraphicsPipeline(*gpu, options), std::invalid_argument);
    }
    SECTION("push-constant bytes not a multiple of 4: std::invalid_argument") {
        for (const std::uint32_t bytes : {1u, 2u, 3u, 17u, 18u, 19u}) {
            INFO(bytes << " bytes");
            GraphicsPipelineOptions options = good;
            options.push_constant_bytes = bytes;
            CHECK_THROWS_AS(GraphicsPipeline(*gpu, options), std::invalid_argument);
        }
    }
    SECTION("push-constant bytes above maxPushConstantsSize: std::invalid_argument") {
        for (const std::uint32_t bytes : {max_push + 4, max_push * 2, UINT32_MAX - 3}) {
            INFO(bytes << " bytes");
            GraphicsPipelineOptions options = good;
            options.push_constant_bytes = bytes;
            CHECK_THROWS_AS(GraphicsPipeline(*gpu, options), std::invalid_argument);
        }
    }
}

TEST_CASE("offscreen: push_constants rejects more bytes than the range, or not a multiple of 4",
          "[gpu]") {
    const std::unique_ptr<Device> gpu = make_device();
    OffscreenTarget target(*gpu, 64, 64);
    const GraphicsPipeline pipeline(*gpu, triangle_options(target)); // a 16-byte range

    // Each is thrown while recording, from inside the draw function, and propagates from
    // render(); it must be thrown before vkCmdPushConstants, or the layer would report it.
    auto render_pushing = [&](std::size_t bytes) {
        const std::vector<std::byte> data(bytes);
        static_cast<void>(target.render([&](VkCommandBuffer commands) {
            pipeline.bind(commands);
            pipeline.push_constants(commands, std::span<const std::byte>(data));
            vkCmdDraw(commands, 3, 1, 0, 0);
        }));
    };

    SECTION("more bytes than the range") {
        CHECK_THROWS_AS(render_pushing(colour_bytes + 4), std::invalid_argument);
        CHECK_THROWS_AS(render_pushing(256), std::invalid_argument);
    }
    SECTION("a size that is not a multiple of 4") {
        CHECK_THROWS_AS(render_pushing(6), std::invalid_argument);
        CHECK_THROWS_AS(render_pushing(15), std::invalid_argument);
    }
    SECTION("a value larger than the range, through the template") {
        const std::array<float, 8> too_large{};
        CHECK_THROWS_AS(static_cast<void>(target.render([&](VkCommandBuffer commands) {
                            pipeline.bind(commands);
                            pipeline.push_constants(commands, too_large);
                        })),
                        std::invalid_argument);
    }
    SECTION("exactly the range is accepted") {
        CHECK_NOTHROW(render_pushing(colour_bytes));
    }

    // The target still renders after the rejected pushes.
    const cs::Image image = target.render(draw_triangle(pipeline, red), clear_to(0, 0, 0));
    CHECK(same(image.at(32, 32), 1.0f, 0.0f, 0.0f));
}

// ---- Header: the depth test keeps fragments nearer than the depth already there (LESS) ---------

TEST_CASE("offscreen: with the depth test, a clear depth nearer than the triangle hides it",
          "[gpu]") {
    const std::unique_ptr<Device> gpu = make_device();
    OffscreenTarget target(*gpu, 64, 64);
    const ClearValues near_clear = clear_to(0.0f, 0.0f, 0.5f, 1.0f, 0.25f);
    const ClearValues far_clear = clear_to(0.0f, 0.0f, 0.5f, 1.0f, 1.0f);

    // The triangle is at depth 0.5. 0.25 and 1.0 are far from it in D32 and in D24 alike, so the
    // depth format's precision does not matter. The colours are exact in half float, as above.
    SECTION("depth test on") {
        const GraphicsPipeline pipeline(*gpu, triangle_options(target));
        const cs::Image hidden = target.render(draw_triangle(pipeline, red), near_clear);
        INFO("hidden centre " << describe(hidden.at(32, 32)));
        CHECK(same(hidden.at(32, 32), 0.0f, 0.0f, 0.5f));

        const cs::Image drawn = target.render(draw_triangle(pipeline, red), far_clear);
        INFO("drawn centre " << describe(drawn.at(32, 32)));
        CHECK(same(drawn.at(32, 32), 1.0f, 0.0f, 0.0f));
        CHECK(same(drawn.at(0, 0), 0.0f, 0.0f, 0.5f));
    }
    SECTION("depth test off: the triangle is drawn whatever the depth") {
        GraphicsPipelineOptions options = triangle_options(target);
        options.depth_test = false;
        const GraphicsPipeline pipeline(*gpu, options);
        const cs::Image image = target.render(draw_triangle(pipeline, red), near_clear);
        INFO("centre " << describe(image.at(32, 32)));
        CHECK(same(image.at(32, 32), 1.0f, 0.0f, 0.0f));
    }
}
