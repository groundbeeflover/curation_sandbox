// SPDX-License-Identifier: Apache-2.0

// The Standard tier against the engine on the skeleton project (issue #78, M1-22): the lux the
// tier's illuminance output reads on the chart, against engine::lux_at's direct value at the same
// point, and the skeleton rendered from its own viewpoint at 256 x 144. It belongs here because
// gpu may depend on neither doc nor engine (blueprint section 7).
//
// The Standard tier has no shadows yet (M2-26). Nothing shadows the chart's centre from the spot,
// so there the two must agree; the frame's shadow on the chart's top edge is left alone.
//
// Validation messages fail these tests through gpu_validation.cpp's listener; devices come from
// libs/gpu/tests/gpu_test.hpp.

#include "../../libs/gpu/tests/gpu_test.hpp"

#include <cs/core/image.hpp>
#include <cs/core/image_io.hpp>
#include <cs/doc/document.hpp>
#include <cs/doc/project_io.hpp>
#include <cs/engine/lux.hpp>
#include <cs/engine/scene.hpp>
#include <cs/gpu/device.hpp>
#include <cs/gpu/scene.hpp>
#include <cs/gpu/standard_tier.hpp>
#include <cs/world/compile.hpp>
#include <cs/world/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>

using Catch::Matchers::WithinRel;
using glm::dvec3;

namespace {

// The ADR 0002 tolerance for the Standard tier's direct light against the engine, and the
// issue's: 2%.
constexpr double direct_tolerance = 0.02;

// What the two should actually agree to. Both evaluate I cos / d^2 from the same spot at the same
// point, the tier in float (a few 1e-6) from its profile table (within 1e-4 of the peak; the
// chart's centre is at about a third of the peak), so 1e-3 leaves room for both and still fails a
// wrong point, normal or intensity that the 2% would let through.
constexpr double agreement = 1e-3;

std::filesystem::path skeleton_folder() {
    return std::filesystem::path(CS_SOURCE_DIR) / "tests" / "scenes" / "skeleton";
}

cs::world::World skeleton_world() {
    const cs::doc::Document document = cs::doc::load_project_folder(skeleton_folder());
    cs::world::World world = cs::world::compile(document);
    REQUIRE(world.problems.empty());
    REQUIRE(world.exhibits.size() == 1);
    REQUIRE(world.lights.size() == 1);
    REQUIRE(world.cameras.size() == 1);
    return world;
}

// A perspective camera's axes as the engine's path tracer makes them (standard_tier.hpp says
// the tier reads cameras the same way).
struct CameraAxes {
    dvec3 forward;
    dvec3 right;
    dvec3 up;
    double half_width;
    double half_height;
};

CameraAxes axes_of(const cs::world::Camera& camera, int width, int height) {
    CameraAxes axes;
    axes.forward = glm::normalize(camera.forward);
    axes.right = glm::normalize(glm::cross(axes.forward, glm::normalize(camera.up)));
    axes.up = glm::cross(axes.right, axes.forward);
    axes.half_height = std::tan(0.5 * camera.vertical_fov_rad);
    axes.half_width = axes.half_height * width / height;
    return axes;
}

// The pixel whose centre is nearest to where a point appears in the image.
glm::ivec2 nearest_pixel(const cs::world::Camera& camera, int width, int height, dvec3 point) {
    const CameraAxes axes = axes_of(camera, width, height);
    const dvec3 d = point - camera.position_m;
    const double depth = glm::dot(d, axes.forward);
    const double sx = glm::dot(d, axes.right) / depth / axes.half_width;
    const double sy = glm::dot(d, axes.up) / depth / axes.half_height;
    return {static_cast<int>(std::lround((sx + 1.0) * 0.5 * width - 0.5)),
            static_cast<int>(std::lround((1.0 - sy) * 0.5 * height - 0.5))};
}

// Where the ray through the centre of pixel (x, y) meets the plane through `on` with normal n.
dvec3 seen_on_plane(const cs::world::Camera& camera, int width, int height, glm::ivec2 pixel,
                    dvec3 on, dvec3 n) {
    const CameraAxes axes = axes_of(camera, width, height);
    const double sx = (2.0 * (pixel.x + 0.5) / width - 1.0) * axes.half_width;
    const double sy = (1.0 - 2.0 * (pixel.y + 0.5) / height) * axes.half_height;
    const dvec3 direction = glm::normalize(axes.forward + sx * axes.right + sy * axes.up);
    const double t = glm::dot(on - camera.position_m, n) / glm::dot(direction, n);
    return camera.position_m + t * direction;
}

float lux_at(const cs::gpu::StandardImages& images, glm::ivec2 pixel) {
    return images.illuminance_lux.at(static_cast<std::size_t>(pixel.y) * images.radiance.width() +
                                     pixel.x);
}

} // namespace

TEST_CASE("standard vs engine: the skeleton's chart centre reads within 2% of engine::lux_at's "
          "direct value",
          "[integration][gpu][accuracy]") {
    const cs::world::World world = skeleton_world();
    const cs::world::Exhibit& chart = world.exhibits.front();

    // The engine's reading at the chart's centre, facing out of the wall, as a curator would hold
    // a meter against it. Only the direct part is compared; it does not depend on samples.
    const cs::engine::Scene engine_scene(world);
    cs::engine::LuxOptions options;
    options.samples = 16;
    const cs::engine::LuxReading reading =
        cs::engine::lux_at(engine_scene, chart.face_centre_m, chart.face_normal, options);
    // The README's number, from cs-render (M1-17): about 1009 lux. Loosely, as a guard that the
    // compiled scene is the one described.
    CHECK_THAT(reading.direct_lux.value, WithinRel(1009.0, 0.01));

    // From the skeleton's viewpoint, aimed exactly at the chart's centre: with odd sides, the
    // centre pixel's ray passes through it.
    cs::world::Camera camera = world.cameras.front();
    camera.forward = glm::normalize(chart.face_centre_m - camera.position_m);
    camera.up = {0.0, 0.0, 1.0};
    constexpr int width = 257;
    constexpr int height = 145;
    const glm::ivec2 centre{width / 2, height / 2};
    REQUIRE(nearest_pixel(camera, width, height, chart.face_centre_m) == centre);

    const std::unique_ptr<cs::gpu::Device> gpu = cs::gpu::test::make_device();
    const cs::gpu::Scene scene(*gpu, world,
                               cs::gpu::project_texture_loader(skeleton_folder() / "assets"));
    CHECK(scene.problems().empty());
    const cs::gpu::StandardTier tier(*gpu, scene, world.lights, width, height);
    const cs::gpu::StandardImages images = tier.render(camera);

    const float lux = lux_at(images, centre);
    INFO("Standard tier " << lux << " lux, engine " << reading.direct_lux.value << " lux");
    CHECK_THAT(lux, WithinRel(reading.direct_lux.value, direct_tolerance));
    CHECK_THAT(lux, WithinRel(reading.direct_lux.value, agreement));
}

TEST_CASE("standard vs engine: the skeleton renders from its viewpoint at 256 x 144, its chart "
          "lit as the engine lights it",
          "[integration][gpu][accuracy]") {
    const cs::world::World world = skeleton_world();
    const cs::world::Exhibit& chart = world.exhibits.front();
    const cs::world::Camera& camera = world.cameras.front(); // "Opposite the chart", EV100 8
    constexpr int width = 256;
    constexpr int height = 144;

    const std::unique_ptr<cs::gpu::Device> gpu = cs::gpu::test::make_device();
    const cs::gpu::Scene scene(*gpu, world,
                               cs::gpu::project_texture_loader(skeleton_folder() / "assets"));
    CHECK(scene.problems().empty());
    const cs::gpu::StandardTier tier(*gpu, scene, world.lights, width, height);
    const cs::gpu::StandardImages images = tier.render(camera);
    REQUIRE(images.radiance.width() == width);
    REQUIRE(images.radiance.height() == height);

    // Light, and nothing that is not light.
    std::size_t lit = 0;
    std::size_t bad = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const cs::Rgb& p = images.radiance.at(x, y);
            const float lux = lux_at(images, {x, y});
            if (!(std::isfinite(p.r) && std::isfinite(p.g) && std::isfinite(p.b) &&
                  std::isfinite(lux) && p.r >= 0.0f && p.g >= 0.0f && p.b >= 0.0f && lux >= 0.0f)) {
                ++bad;
            }
            if (lux > 0.0f) {
                ++lit;
            }
        }
    }
    CHECK(bad == 0);
    CHECK(lit > std::size_t{width} * height / 10);

    // The pixel nearest the chart's centre: the point it sees, on the chart, read by the engine.
    const glm::ivec2 pixel = nearest_pixel(camera, width, height, chart.face_centre_m);
    const dvec3 point =
        seen_on_plane(camera, width, height, pixel, chart.face_centre_m, chart.face_normal);
    const glm::dvec2 on_face{glm::dot(point - chart.face_centre_m, chart.face_u),
                             glm::dot(point - chart.face_centre_m, chart.face_v)};
    REQUIRE(on_face.x > chart.visible.min_m.x);
    REQUIRE(on_face.x < chart.visible.max_m.x);
    REQUIRE(on_face.y > chart.visible.min_m.y);
    REQUIRE(on_face.y < chart.visible.max_m.y);
    const cs::engine::Scene engine_scene(world);
    const double expected = cs::engine::direct_lux(engine_scene, point, chart.face_normal);
    INFO("pixel (" << pixel.x << ", " << pixel.y << "): Standard tier " << lux_at(images, pixel)
                   << " lux, engine " << expected << " lux");
    CHECK_THAT(lux_at(images, pixel), WithinRel(expected, direct_tolerance));
    CHECK_THAT(lux_at(images, pixel), WithinRel(expected, agreement));

    // The picture for the pull request, tone mapped on the CPU with libs/core (M1-44 brings the
    // GPU's own pass): the build tree's standard-out/.
    const std::filesystem::path out = std::filesystem::current_path() / "standard-out";
    std::filesystem::create_directories(out);
    const std::filesystem::path png = out / "skeleton-256x144.png";
    cs::write_png(images.radiance, png, camera.ev100.value_or(8.0));
    std::cout << "Standard tier render of the skeleton: " << png.string() << '\n';
}
