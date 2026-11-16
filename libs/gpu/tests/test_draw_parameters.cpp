// SPDX-License-Identifier: Apache-2.0

// The device enables shaderDrawParameters (Vulkan 1.1 core), so shaders may use SV_VertexID,
// which Slang compiles as VertexIndex minus BaseVertex (issue M1-21, the owner's addition from
// M1-20). Scenes draw each mesh with its own vertexOffset and read their vertices by SV_VertexID
// (scene.hpp), so this is what makes their draws land on the right vertices.
//
// The shader fixture is libs/gpu/shaders/base_vertex.slang: no vertex input, a 16-byte push
// constant float4 colour like triangle.slang's, and a vertex shader that reads SV_VertexID and
// places vertices 0, 1 and 2 where triangle.slang does ((-0.5, -0.5), (0.5, -0.5), (0, 0.5) in
// clip space, at depth 0.5) and any vertex from 3 on outside the clip volume. So a draw of three
// vertices from firstVertex 3 shows the triangle only if SV_VertexID counts from the draw's first
// vertex (BaseVertex is firstVertex for a non-indexed draw); if it were VertexIndex as it is, the
// vertices would be 3, 4 and 5 and nothing would be drawn.
//
// Without the feature, the pipeline's SPIR-V declares the DrawParameters capability on a device
// that has not enabled it, which the validation layer reports, and the harness (main.cpp) fails
// the test on.

#include "gpu_test.hpp"

#include <cs/core/image.hpp>
#include <cs/gpu/device.hpp>
#include <cs/gpu/offscreen.hpp>
#include <cs/gpu/pipeline.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>

using cs::gpu::ClearValues;
using cs::gpu::Device;
using cs::gpu::GraphicsPipeline;
using cs::gpu::GraphicsPipelineOptions;
using cs::gpu::OffscreenTarget;
using cs::gpu::test::make_device;

namespace {

using Colour = std::array<float, 4>; // the push constant: float4 colour, linear RGBA
constexpr Colour green{0.0f, 1.0f, 0.0f, 1.0f};

GraphicsPipelineOptions base_vertex_options(const OffscreenTarget& target) {
    GraphicsPipelineOptions options;
    options.shader = "base_vertex";
    options.colour_format = OffscreenTarget::colour_format;
    options.depth_format = target.depth_format();
    options.push_constant_bytes = sizeof(Colour);
    return options;
}

// Three vertices from first_vertex, one instance.
OffscreenTarget::DrawFunction draw_from(const GraphicsPipeline& pipeline,
                                        std::uint32_t first_vertex) {
    return [&pipeline, first_vertex](VkCommandBuffer commands) {
        pipeline.bind(commands);
        pipeline.push_constants(commands, green);
        vkCmdDraw(commands, 3, 1, first_vertex, 0);
    };
}

ClearValues clear_blue() {
    ClearValues clear;
    clear.colour = {0.0f, 0.0f, 0.25f, 1.0f};
    return clear;
}

std::string describe(const cs::Rgb& pixel) {
    std::ostringstream text;
    text << '(' << pixel.r << ", " << pixel.g << ", " << pixel.b << ')';
    return text.str();
}

// Exact, as in test_offscreen.cpp: the fragment shader writes the push constant unchanged, 0, 1
// and 0.25 are exact in half float, and there is no blending, MSAA or filtering, so a pixel is
// exactly the triangle's colour or exactly the clear colour.
bool same(const cs::Rgb& pixel, float r, float g, float b) {
    return pixel.r == r && pixel.g == g && pixel.b == b;
}

void check_triangle_drawn(const cs::Image& image) {
    REQUIRE(image.width() == 64);
    REQUIRE(image.height() == 64);
    INFO("centre " << describe(image.at(32, 32)));
    CHECK(same(image.at(32, 32), 0.0f, 1.0f, 0.0f));
    for (const auto& [x, y] :
         std::array<std::array<int, 2>, 4>{{{0, 0}, {63, 0}, {0, 63}, {63, 63}}}) {
        INFO("corner (" << x << ", " << y << "): " << describe(image.at(x, y)));
        CHECK(same(image.at(x, y), 0.0f, 0.0f, 0.25f));
    }
}

} // namespace

TEST_CASE("device: shaderDrawParameters is enabled, so SV_VertexID counts from a non-zero first "
          "vertex and the triangle is drawn",
          "[gpu][device]") {
    const std::unique_ptr<Device> gpu = make_device();
    OffscreenTarget target(*gpu, 64, 64);
    const GraphicsPipeline pipeline(*gpu, base_vertex_options(target));

    SECTION("firstVertex 3: VertexIndex 3, 4, 5 and BaseVertex 3 give SV_VertexID 0, 1, 2") {
        check_triangle_drawn(target.render(draw_from(pipeline, 3), clear_blue()));
    }
    SECTION("a larger first vertex: 1000") {
        check_triangle_drawn(target.render(draw_from(pipeline, 1000), clear_blue()));
    }
    SECTION("the control: firstVertex 0 draws the same triangle") {
        check_triangle_drawn(target.render(draw_from(pipeline, 0), clear_blue()));
    }
}

TEST_CASE("device: base_vertex.slang puts vertices from SV_VertexID 3 on outside the view",
          "[gpu][device]") {
    // The fixture's other half, so that the test above means what it says: six vertices from
    // firstVertex 0 are SV_VertexID 0 to 5, the triangle and three vertices outside the clip
    // volume, and the image is the triangle's alone. Were vertices 3 to 5 not outside, a
    // second triangle would cover a corner or the image would differ from the three-vertex one.
    const std::unique_ptr<Device> gpu = make_device();
    OffscreenTarget target(*gpu, 64, 64);
    const GraphicsPipeline pipeline(*gpu, base_vertex_options(target));

    const cs::Image image = target.render(
        [&pipeline](VkCommandBuffer commands) {
            pipeline.bind(commands);
            pipeline.push_constants(commands, green);
            vkCmdDraw(commands, 6, 1, 0, 0);
        },
        clear_blue());
    check_triangle_drawn(image);

    const cs::Image three = target.render(draw_from(pipeline, 0), clear_blue());
    int differences = 0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const cs::Rgb& a = image.at(x, y);
            const cs::Rgb& b = three.at(x, y);
            if (!same(a, b.r, b.g, b.b)) {
                ++differences;
            }
        }
    }
    CHECK(differences == 0);
}
