// SPDX-License-Identifier: Apache-2.0
//
// ViewRenderer (issue #82): what the viewport draws, without Qt widgets. Headless, it renders the
// Standard tier through the tone-mapping pass and reads the display image back, which is how the
// viewport draws on the offscreen platform.

#include "ui_test.hpp"
#include "viewport/gpu_context.hpp"
#include "viewport/view_renderer.hpp"

#include <cs/doc/project_io.hpp>
#include <cs/gpu/device.hpp>
#include <cs/world/compile.hpp>
#include <cs/world/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using Catch::Matchers::WithinAbs;

// The bytes of one pixel of an RGBA8 image, row 0 at the top.
const std::uint8_t* pixel(const std::vector<std::uint8_t>& rgba, std::uint32_t width,
                          std::uint32_t x, std::uint32_t y) {
    return rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
}

std::uint64_t sum_rgb(const std::vector<std::uint8_t>& rgba) {
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        sum += rgba[i] + rgba[i + 1] + rgba[i + 2];
    }
    return sum;
}

} // namespace

// Acceptance criterion (the viewport's camera with no viewpoint): the default camera stands at the
// origin at eye height, looking along x with a 50 degree vertical field of view. Exact to 1e-12:
// the values are constants, the tolerance only absorbs a conversion from degrees.
TEST_CASE("default_camera: at the origin at eye height, looking along x, 50 degrees", "[ui]") {
    const cs::world::Camera camera = cs::ui::default_camera();
    CHECK(camera.projection == cs::world::Projection::perspective);
    CHECK_FALSE(camera.entity.has_value());
    CHECK(camera.position_m.x == 0.0);
    CHECK(camera.position_m.y == 0.0);
    // Eye height: the document's default, ProjectSettings::eye_height_m = 1.60 m.
    CHECK_THAT(camera.position_m.z, WithinAbs(1.6, 1e-12));
    CHECK(camera.forward.x == 1.0);
    CHECK(camera.forward.y == 0.0);
    CHECK(camera.forward.z == 0.0);
    CHECK(camera.up.x == 0.0);
    CHECK(camera.up.y == 0.0);
    CHECK(camera.up.z == 1.0);
    // 50 degrees = 50 * pi / 180 rad.
    CHECK_THAT(camera.vertical_fov_rad, WithinAbs(50.0 * std::acos(-1.0) / 180.0, 1e-12));
}

// Acceptance criterion: without a window surface the viewport renders into an offscreen target
// (ViewRenderer::render, the headless view's draw), the Standard tier through the tone-mapping
// pass; nothing is ready to draw before a world and a size with area.
TEST_CASE("ViewRenderer: renders the skeleton headless at the size asked; not ready before a "
          "world or with size 0",
          "[ui][gpu]") {
    std::unique_ptr<cs::ui::GpuContext> gpu = cs::ui::test::make_headless_gpu();
    REQUIRE_FALSE(gpu->device().presentation());
    const std::filesystem::path skeleton = cs::ui::test::skeleton_folder();
    const cs::world::World world = cs::world::compile(cs::doc::load_project_folder(skeleton));
    REQUIRE_FALSE(world.cameras.empty());
    {
        cs::ui::ViewRenderer renderer(gpu->device());
        CHECK_FALSE(renderer.ready());
        CHECK(renderer.ev100() == 8.0);
        CHECK(renderer.camera() == cs::ui::default_camera());

        // A size but no world: nothing to draw.
        renderer.set_size(64, 36);
        CHECK(renderer.width() == 64);
        CHECK(renderer.height() == 36);
        CHECK_FALSE(renderer.ready());

        // The world: its painting's image is in the project's assets, so none falls back.
        const std::vector<std::string> problems = renderer.set_world(world, skeleton / "assets");
        CHECK(problems.empty());
        REQUIRE(renderer.ready());

        renderer.set_camera(world.cameras.front());
        renderer.set_ev100(8.0);
        const std::vector<std::uint8_t> image = renderer.render();
        REQUIRE(image.size() == std::size_t{64} * 36 * 4);
        // A = 255 everywhere.
        bool opaque = true;
        for (std::size_t i = 3; i < image.size(); i += 4) {
            opaque = opaque && image[i] == 255;
        }
        CHECK(opaque);
        // From the viewpoint opposite the chart, the centre is the lit chart (about 1000 lux on
        // paper whose darkest patch reflects 0.7 %: >= 2 nits, about 15 in sRGB at EV100 8), so
        // it is not black.
        const std::uint8_t* centre = pixel(image, 64, 32, 18);
        CHECK(int{centre[0]} + int{centre[1]} + int{centre[2]} > 0);

        // A lower EV100 is a longer exposure: 4 stops lower is 16 times brighter before the tone
        // curve, which is monotonic, so the image is brighter in sum.
        renderer.set_ev100(4.0);
        CHECK(renderer.ev100() == 4.0);
        const std::vector<std::uint8_t> brighter = renderer.render();
        REQUIRE(brighter.size() == image.size());
        CHECK(sum_rgb(brighter) > sum_rgb(image));

        // Size 0 drops the tier: not ready, until a size with area comes.
        renderer.set_size(0, 36);
        CHECK_FALSE(renderer.ready());
        renderer.set_size(0, 0);
        CHECK_FALSE(renderer.ready());
        renderer.set_size(32, 20);
        REQUIRE(renderer.ready());
        CHECK(renderer.render().size() == std::size_t{32} * 20 * 4);

        // The same size changes nothing.
        renderer.set_size(32, 20);
        CHECK(renderer.ready());
        CHECK(renderer.width() == 32);
        CHECK(renderer.height() == 20);
    }
    cs::ui::test::require_no_validation_messages(*gpu, gpu->validation_status());
}
