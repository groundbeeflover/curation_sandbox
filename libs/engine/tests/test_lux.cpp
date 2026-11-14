// SPDX-License-Identifier: Apache-2.0

// The lux meter (lux.hpp), from issue #68 (M1-15) and the owner's comment on it: direct
// illuminance by shadow rays to every luminaire, total illuminance adding cosine-weighted
// indirect paths, each with a standard error; checked against the inverse-square and cosine
// laws, a uniform cone, a spot lying in the ceiling's plane, the skeleton scene's chart, and the
// integrating sphere for the indirect part. Results must not depend on the thread count.
//
// Every world is built here by hand from geom's meshes. The engine may not read the document
// (blueprint section 7), so the skeleton below is assembled from the numbers of
// tests/scenes/skeleton/README.md and libs/world/tests/test_skeleton_world.cpp; the check on the
// truly compiled document belongs in tests/integration/ (M1-34).

#include <cs/engine/lux.hpp>
#include <cs/engine/scene.hpp>

#include <cs/core/colour.hpp>
#include <cs/core/photometry.hpp>
#include <cs/core/thread_pool.hpp>
#include <cs/geom/box_frame.hpp>
#include <cs/geom/mesh.hpp>
#include <cs/geom/painting.hpp>
#include <cs/geom/room.hpp>
#include <cs/geom/wall_face.hpp>
#include <cs/light/distribution.hpp>
#include <cs/light/luminaire.hpp>
#include <cs/world/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using cs::engine::direct_lux;
using cs::engine::light_visible;
using cs::engine::lux_at;
using cs::engine::LuxOptions;
using cs::engine::LuxReading;
using cs::engine::Scene;
using glm::dvec2;
using glm::dvec3;

namespace geom = cs::geom;
namespace light = cs::light;
namespace world = cs::world;

namespace {

constexpr double pi = std::numbers::pi;
constexpr double degrees = pi / 180.0;
constexpr double not_a_number = std::numeric_limits<double>::quiet_NaN();
constexpr double inf = std::numeric_limits<double>::infinity();

// The issue's tolerance for every analytic check of the direct illuminance: 1%.
constexpr double issue_tolerance = 0.01;

// ---- Lights ------------------------------------------------------------------------------------

// An ideal point source: the same intensity in every direction, so 4 pi times it in lumens.
class Isotropic final : public light::Distribution {
  public:
    explicit Isotropic(double intensity_cd) : intensity_cd_(intensity_cd) {}

    double intensity_cd(glm::dvec3 direction) const override {
        if (std::isnan(direction.x) || std::isnan(direction.y) || std::isnan(direction.z)) {
            return not_a_number;
        }
        return intensity_cd_;
    }
    double total_lumens() const override { return 4.0 * pi * intensity_cd_; }

  private:
    double intensity_cd_;
};

world::Light make_light(dvec3 position_m, dvec3 aim_m,
                        std::shared_ptr<const light::Distribution> distribution,
                        std::shared_ptr<const light::GenericSpot> spot) {
    return world::Light{.entity = world::Id{},
                        .luminaire = light::Luminaire(position_m, aim_m, std::move(distribution)),
                        .spot = std::move(spot),
                        .colour_temperature_k = 3000.0};
}

// An isotropic light. World::Light also carries the distribution as a generic spot for the
// shaders: a uniform cone over the whole sphere (beam = field = 360 degrees), whose intensity is
// lumens / (2 pi (1 - cos pi)) = 4 pi I / (4 pi) = I, the same light.
world::Light isotropic_light(dvec3 position_m, double intensity_cd) {
    return make_light(
        position_m, position_m + dvec3(0.0, 0.0, -1.0),
        std::make_shared<const Isotropic>(intensity_cd),
        std::make_shared<const light::GenericSpot>(4.0 * pi * intensity_cd, 2.0 * pi, 2.0 * pi));
}

world::Light spot_light(dvec3 position_m, dvec3 aim_m,
                        const std::shared_ptr<const light::GenericSpot>& spot) {
    return make_light(position_m, aim_m, spot, spot);
}

// ---- Meshes ------------------------------------------------------------------------------------

// A rectangle centred on `centre`, spanning +-half_u and +-half_v, facing cross(half_u, half_v):
// four vertices, two triangles wound counter-clockwise seen from its front.
geom::Mesh rectangle(dvec3 centre, dvec3 half_u, dvec3 half_v) {
    const dvec3 normal = glm::normalize(glm::cross(half_u, half_v));
    geom::Mesh mesh;
    mesh.positions = {centre - half_u - half_v, centre + half_u - half_v, centre + half_u + half_v,
                      centre - half_u + half_v};
    mesh.normals.assign(4, normal);
    mesh.uvs = {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

// A floor at z = 0, 1000 m on a side, facing up: "large" in the issue's sense, since a sensor
// 2 m above its centre sees it out to 89.8 degrees from the vertical. With no other surface,
// nothing the floor reflects comes back to a sensor on it facing up.
geom::Mesh large_floor() {
    return rectangle(dvec3(0.0), dvec3(500.0, 0.0, 0.0), dvec3(0.0, 500.0, 0.0));
}

// A closed polyhedron inscribed in the sphere of radius_m about the origin: n_lat bands of
// latitude, n_lon segments of longitude, flat triangles each with its own vertices and its
// normal, facing in (towards the centre). Corners are computed once and repeated bit for bit,
// so it is watertight.
geom::Mesh faceted_sphere(double radius_m, int n_lon, int n_lat) {
    std::vector<dvec3> grid(static_cast<std::size_t>((n_lat + 1) * n_lon));
    auto at = [&](int i, int j) -> const dvec3& {
        return grid[static_cast<std::size_t>(i * n_lon + j % n_lon)];
    };
    for (int i = 0; i <= n_lat; ++i) {
        const double theta = pi * i / n_lat;
        for (int j = 0; j < n_lon; ++j) {
            const double phi = 2.0 * pi * j / n_lon;
            dvec3 p = radius_m * dvec3(std::sin(theta) * std::cos(phi),
                                       std::sin(theta) * std::sin(phi), std::cos(theta));
            if (i == 0) {
                p = dvec3(0.0, 0.0, radius_m);
            } else if (i == n_lat) {
                p = dvec3(0.0, 0.0, -radius_m);
            }
            grid[static_cast<std::size_t>(i * n_lon + j)] = p;
        }
    }

    geom::Mesh mesh;
    auto add = [&](dvec3 a, dvec3 b, dvec3 c) {
        dvec3 n = glm::cross(b - a, c - a);
        if (glm::dot(n, a + b + c) > 0.0) { // facing out: turn it to face the centre
            std::swap(b, c);
            n = -n;
        }
        n = glm::normalize(n);
        const auto base = static_cast<std::uint32_t>(mesh.positions.size());
        for (const dvec3& p : {a, b, c}) {
            mesh.positions.push_back(p);
            mesh.normals.push_back(n);
            mesh.uvs.push_back(dvec2(0.0));
        }
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2});
    };
    for (int i = 0; i < n_lat; ++i) {
        for (int j = 0; j < n_lon; ++j) {
            const dvec3& a = at(i, j);
            const dvec3& b = at(i, j + 1);
            const dvec3& c = at(i + 1, j + 1);
            const dvec3& d = at(i + 1, j);
            if (i != 0) {
                add(a, b, c); // at the north pole a and b are the pole
            }
            if (i != n_lat - 1) {
                add(a, c, d); // at the south pole c and d are
            }
        }
    }
    return mesh;
}

// ---- Worlds ------------------------------------------------------------------------------------

// Adds a mesh in world coordinates (identity transform) with a Lambert material of its own.
void add_lambert(world::World& w, geom::Mesh mesh, double albedo) {
    world::Instance instance;
    instance.part = world::Part::room;
    instance.mesh = static_cast<std::uint32_t>(w.meshes.size());
    instance.material = static_cast<std::uint32_t>(w.materials.size());
    w.meshes.push_back(std::move(mesh));
    w.materials.emplace_back(world::LambertMaterial{dvec3(albedo)});
    w.instances.push_back(instance);
}

world::World floor_world(double albedo = 0.5) {
    world::World w;
    add_lambert(w, large_floor(), albedo);
    return w;
}

// The skeleton's room: 6 x 4 x 3.5 m, closed, facing in, white gallery paint.
world::World closed_room() {
    world::World w;
    add_lambert(w, geom::room_mesh(6.0, 4.0, 3.5), world::default_wall_albedo);
    return w;
}

std::shared_ptr<const light::GenericSpot> skeleton_spot() {
    // The skeleton's fixture type: 800 lm, beam 15 degrees and field 25 degrees (full angles).
    return std::make_shared<const light::GenericSpot>(800.0, 15.0 * degrees, 25.0 * degrees);
}

// The closed room with a spot in the ceiling's plane over the middle of the floor, aimed
// straight down: the owner's case, where a shadow ray to the light ends on the ceiling.
world::World room_with_ceiling_spot() {
    world::World w = closed_room();
    w.lights.push_back(spot_light({3.0, 2.0, 3.5}, {3.0, 2.0, 0.0}, skeleton_spot()));
    return w;
}

// |actual - expected| within the issue's 1%, and within 3 standard errors of the reading plus a
// rounding allowance. Direct readings from point lights are sums, not estimates (lux.hpp), so
// their standard error is 0 and the second check asks that they be right to 1e-4, far tighter
// than the 1% and far looser than double rounding.
void check_reading(const cs::engine::Estimate& reading, double expected) {
    INFO("reading " << reading.value << " +- " << reading.standard_error << ", expected "
                    << expected);
    CHECK_THAT(reading.value, WithinRel(expected, issue_tolerance));
    CHECK(std::abs(reading.value - expected) <=
          3.0 * reading.standard_error + 1e-4 * std::abs(expected));
}

double sample_mean(const std::vector<double>& xs) {
    double sum = 0.0;
    for (const double x : xs) {
        sum += x;
    }
    return sum / static_cast<double>(xs.size());
}

} // namespace

// ---- Shadow rays -------------------------------------------------------------------------------

TEST_CASE("shadow_gap_m: 1e-4 of the distance plus 1e-5 m, 0.36 mm over 3.5 m", "[engine][lux]") {
    // lux.hpp: "1e-4 of d plus 1e-5 m, so 0.35 mm over the 3.5 m from the skeleton's ceiling spot
    // to the floor" (3.5e-4 + 1e-5 = 3.6e-4 m).
    static_assert(cs::engine::shadow_gap_m(0.0) == 1e-5);
    CHECK_THAT(cs::engine::shadow_gap_m(3.5), WithinRel(3.6e-4, 1e-12));
    CHECK_THAT(cs::engine::shadow_gap_m(2.0), WithinRel(2.1e-4, 1e-12));
    CHECK_THAT(cs::engine::shadow_gap_m(100.0), WithinRel(1.001e-2, 1e-12));
    // Always less than the distance itself for anything a room holds.
    for (const double d : {0.01, 0.1, 1.0, 10.0, 100.0}) {
        CHECK(cs::engine::shadow_gap_m(d) < d);
    }
}

TEST_CASE("light_visible: a surface between the point and the light hides it, and one beside "
          "the segment does not",
          "[engine][lux]") {
    world::World w;
    // A 2 x 2 m blocker 1 m above the origin, facing up or down: both sides block (scene.hpp).
    const bool facing_up = GENERATE(true, false);
    INFO("blocker facing " << (facing_up ? "up" : "down"));
    add_lambert(w,
                rectangle({0.0, 0.0, 1.0}, {1.0, 0.0, 0.0},
                          facing_up ? dvec3(0.0, 1.0, 0.0) : dvec3(0.0, -1.0, 0.0)),
                0.5);
    const Scene scene(w);
    const dvec3 up(0.0, 0.0, 1.0);

    CHECK_FALSE(light_visible(scene, {0.0, 0.0, 0.0}, up, {0.0, 0.0, 2.0}));
    CHECK_FALSE(light_visible(scene, {0.5, -0.5, 0.0}, up, {0.0, 0.0, 2.0}));
    // From (3, 0, 0) to (0, 0, 2) the segment crosses z = 1 at x = 1.5, past the blocker's edge.
    CHECK(light_visible(scene, {3.0, 0.0, 0.0}, up, {0.0, 0.0, 2.0}));
    // A light below the blocker, seen from below it.
    CHECK(light_visible(scene, {0.0, 0.0, 0.0}, up, {0.2, 0.1, 0.5}));
}

TEST_CASE("light_visible: a light on the other side of the surface is not visible",
          "[engine][lux]") {
    const Scene scene(world::World{}); // nothing in the way at all
    CHECK(light_visible(scene, {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, 0.0, 2.0}));
    CHECK_FALSE(light_visible(scene, {0.0, 0.0, 0.0}, {0.0, 0.0, -1.0}, {0.0, 0.0, 2.0}));
    CHECK_FALSE(light_visible(scene, {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, {1.0, 1.0, -0.5}));
}

TEST_CASE("light_visible: a light no further away than the start's offset is not visible",
          "[engine][lux]") {
    const Scene scene(world::World{});
    // At the origin ray_offset_m is 1e-5 m (scene.hpp); a light half that far along the side.
    const double offset = cs::engine::ray_offset_m({0.0, 0.0, 0.0});
    CHECK_FALSE(light_visible(scene, {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, 0.0, 0.5 * offset}));
    CHECK_FALSE(light_visible(scene, {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, 0.0, 0.0}));
}

TEST_CASE("light_visible: a surface within shadow_gap_m of the light, or one the light lies in, "
          "does not hide it",
          "[engine][lux]") {
    // From the origin to a light at (0, 0, 2): d = 2 m, so the gap is 2.1e-4 m (lux.hpp). A
    // horizontal 2 x 2 m sheet at z = 2 - s, centred on the segment. Floats near 2 are spaced
    // 2.4e-7 m, far finer than the margins here (1.1e-4 m and 7.9e-4 m from the gap's edge).
    const double light_z = 2.0;
    struct Case {
        double below_light_m;
        bool visible;
    };
    const Case c = GENERATE(Case{0.0, true},   // the light lies in the sheet: a ceiling
                            Case{1e-4, true},  // 0.1 mm below it, inside the gap
                            Case{1e-3, false}, // 1 mm below it, outside the gap
                            Case{0.5, false}); // half way
    INFO("sheet " << c.below_light_m << " m below the light");
    world::World w;
    add_lambert(w,
                rectangle({0.0, 0.0, light_z - c.below_light_m}, {1.0, 0.0, 0.0},
                          {0.0, -1.0, 0.0}), // facing down, towards the point, as a ceiling
                0.8);
    const Scene scene(w);
    CHECK(light_visible(scene, {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, 0.0, light_z}) == c.visible);
}

// ---- Direct illuminance: the issue's analytic cases --------------------------------------------

TEST_CASE("lux_at: a 1000 cd isotropic point 2 m above a large floor gives 250 lux "
          "direct, within 1% and 3 standard errors",
          "[engine][lux][accuracy]") {
    // E = I cos(theta) / d^2 = 1000 x 1 / 2^2 = 250 lux.
    world::World w = floor_world();
    w.lights.push_back(isotropic_light({0.0, 0.0, 2.0}, 1000.0));
    const Scene scene(w);

    const LuxReading reading =
        lux_at(scene, {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, LuxOptions{.samples = 256, .seed = 7});
    check_reading(reading.direct_lux, 250.0);
    CHECK(reading.direct_lux.standard_error == 0.0); // a point light: a sum, not an estimate

    // Nothing above the floor reflects light back to a sensor on it facing up, so the total is
    // the direct, with no error either: every indirect sample finds nothing.
    check_reading(reading.total_lux, 250.0);
    CHECK_THAT(reading.total_lux.value, WithinRel(reading.direct_lux.value, 1e-12));
    CHECK_THAT(reading.total_lux.standard_error, WithinAbs(0.0, 1e-9));
}

TEST_CASE("lux_at: a sensor 2 m from the light, tilted so the light arrives 60 "
          "degrees from its normal, reads 125 lux",
          "[engine][lux][accuracy]") {
    // E = 1000 x cos 60 / 2^2 = 125 lux, whichever way the sensor is tilted about the vertical.
    world::World w = floor_world();
    w.lights.push_back(isotropic_light({0.0, 0.0, 2.0}, 1000.0));
    const Scene scene(w);

    const double azimuth = GENERATE(0.0, 90.0, 225.0);
    INFO("tilted towards azimuth " << azimuth << " degrees");
    const double tilt = 60.0 * degrees;
    const dvec3 normal(std::sin(tilt) * std::cos(azimuth * degrees),
                       std::sin(tilt) * std::sin(azimuth * degrees), std::cos(tilt));

    CHECK_THAT(direct_lux(scene, {0.0, 0.0, 0.0}, normal), WithinRel(125.0, issue_tolerance));
    const LuxReading reading =
        lux_at(scene, {0.0, 0.0, 0.0}, normal, LuxOptions{.samples = 64, .seed = 3});
    check_reading(reading.direct_lux, 125.0);
    CHECK(reading.direct_lux.standard_error == 0.0);
}

TEST_CASE("lux_at: on the floor, the isotropic point follows I h / r^3 everywhere",
          "[engine][lux][accuracy]") {
    // A point (x, y, 0) on the floor, facing up, under a light at height h = 2: cos(theta) = h / r
    // and d = r, so E = I h / r^3.
    world::World w = floor_world();
    w.lights.push_back(isotropic_light({0.0, 0.0, 2.0}, 1000.0));
    const Scene scene(w);

    for (const dvec2 xy : {dvec2(1.0, 0.0), dvec2(2.0, 2.0), dvec2(-3.0, 1.0), dvec2(0.0, -10.0)}) {
        const double r = std::sqrt(xy.x * xy.x + xy.y * xy.y + 4.0);
        const double expected = 1000.0 * 2.0 / (r * r * r);
        INFO("at (" << xy.x << ", " << xy.y << ")");
        CHECK_THAT(direct_lux(scene, {xy.x, xy.y, 0.0}, {0.0, 0.0, 1.0}),
                   WithinRel(expected, issue_tolerance));
    }
}

TEST_CASE("lux_at: GenericSpot(1000 lm, 30, 30 degrees) straight down from 2 m gives "
          "1167.7 lux",
          "[engine][lux][accuracy]") {
    // A uniform cone of 15 degrees half-angle: I = 1000 / (2 pi (1 - cos 15 deg)) =
    // 1000 / (2 pi x 0.0340742) = 4670.8 cd; E = I / 2^2 = 1167.7 lux.
    const auto cone =
        std::make_shared<const light::GenericSpot>(1000.0, 30.0 * degrees, 30.0 * degrees);
    const double intensity = cs::photometry::uniform_cone_intensity(1000.0, 15.0 * degrees);
    CHECK_THAT(intensity / 4.0, WithinRel(1167.7, 1e-4)); // the issue's figure, to its digits

    world::World w = floor_world();
    w.lights.push_back(spot_light({0.0, 0.0, 2.0}, {0.0, 0.0, 0.0}, cone));
    const Scene scene(w);

    const LuxReading reading =
        lux_at(scene, {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, LuxOptions{.samples = 64, .seed = 11});
    check_reading(reading.direct_lux, 1167.7);
    check_reading(reading.direct_lux, intensity / 4.0);
    CHECK(reading.direct_lux.standard_error == 0.0);
    CHECK_THAT(direct_lux(scene, {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}),
               WithinRel(1167.7, issue_tolerance));
}

TEST_CASE("lux_at: the uniform cone lights the floor as I cos^3 / h^2 inside it, and "
          "not at all outside",
          "[engine][lux][accuracy]") {
    const auto cone =
        std::make_shared<const light::GenericSpot>(1000.0, 30.0 * degrees, 30.0 * degrees);
    const double intensity = cs::photometry::uniform_cone_intensity(1000.0, 15.0 * degrees);
    world::World w = floor_world();
    w.lights.push_back(spot_light({0.0, 0.0, 2.0}, {0.0, 0.0, 0.0}, cone));
    const Scene scene(w);

    // 10 degrees off the axis, inside the 15 degree cone: d = h / cos, cos(theta) = cos, so
    // E = I cos^3 / h^2.
    const double inside = 10.0 * degrees;
    const dvec3 p_inside(2.0 * std::tan(inside), 0.0, 0.0);
    const double c = std::cos(inside);
    CHECK_THAT(direct_lux(scene, p_inside, {0.0, 0.0, 1.0}),
               WithinRel(intensity * c * c * c / 4.0, issue_tolerance));

    // 20 degrees off the axis: outside the cone, which emits nothing there (distribution.hpp).
    const dvec3 p_outside(0.0, 2.0 * std::tan(20.0 * degrees), 0.0);
    CHECK(direct_lux(scene, p_outside, {0.0, 0.0, 1.0}) == 0.0);
}

TEST_CASE("lux_at: a blocker between the light and the sensor makes the direct illuminance 0",
          "[engine][lux]") {
    world::World w = floor_world();
    // A 2 x 2 m sheet 1 m up, between the light at 2 m and the floor; either side down.
    const bool facing_up = GENERATE(true, false);
    INFO("blocker facing " << (facing_up ? "up" : "down"));
    add_lambert(w,
                rectangle({0.0, 0.0, 1.0}, {1.0, 0.0, 0.0},
                          facing_up ? dvec3(0.0, 1.0, 0.0) : dvec3(0.0, -1.0, 0.0)),
                0.5);
    w.lights.push_back(isotropic_light({0.0, 0.0, 2.0}, 1000.0));
    const Scene scene(w);

    const LuxReading shadowed =
        lux_at(scene, {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, LuxOptions{.samples = 64, .seed = 5});
    CHECK(shadowed.direct_lux.value == 0.0);
    CHECK(shadowed.direct_lux.standard_error == 0.0);
    CHECK(direct_lux(scene, {0.4, -0.3, 0.0}, {0.0, 0.0, 1.0}) == 0.0);

    // Outside the shadow, which ends at x = 2 on the floor, the floor is lit as without it:
    // at (3, 0, 0), E = I h / r^3 = 1000 x 2 / 13^1.5 (the segment passes z = 1 at x = 1.5).
    CHECK_THAT(direct_lux(scene, {3.0, 0.0, 0.0}, {0.0, 0.0, 1.0}),
               WithinRel(2000.0 / std::pow(13.0, 1.5), issue_tolerance));
}

TEST_CASE("lux_at: a light behind the sensor, or in its plane, gives no direct light",
          "[engine][lux]") {
    world::World w; // no surfaces: only the sensor's own orientation matters
    w.lights.push_back(isotropic_light({0.0, 0.0, 2.0}, 1000.0));
    const Scene scene(w);
    CHECK(direct_lux(scene, {0.0, 0.0, 0.0}, {0.0, 0.0, -1.0}) == 0.0); // facing away
    CHECK(direct_lux(scene, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}) == 0.0);  // cos(theta) = 0
    CHECK_THAT(direct_lux(scene, {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}),
               WithinRel(250.0, issue_tolerance));
}

TEST_CASE("lux_at: a spot aimed away from the sensor gives no direct light", "[engine][lux]") {
    const auto cone =
        std::make_shared<const light::GenericSpot>(1000.0, 30.0 * degrees, 30.0 * degrees);
    world::World w = floor_world();
    w.lights.push_back(spot_light({0.0, 0.0, 2.0}, {0.0, 0.0, 4.0}, cone)); // pointing up
    const Scene scene(w);
    CHECK(direct_lux(scene, {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}) == 0.0);
}

TEST_CASE("lux_at: the direct light of several luminaires adds up", "[engine][lux][accuracy]") {
    // 1000 cd at 2 m above the origin (250 lux) and 400 cd at (3, 0, 4): d = 5, cos = 4 / 5,
    // E = 400 x 0.8 / 25 = 12.8 lux. Together 262.8 lux.
    world::World w = floor_world();
    w.lights.push_back(isotropic_light({0.0, 0.0, 2.0}, 1000.0));
    w.lights.push_back(isotropic_light({3.0, 0.0, 4.0}, 400.0));
    const Scene scene(w);
    CHECK_THAT(direct_lux(scene, {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}),
               WithinRel(262.8, issue_tolerance));
}

TEST_CASE("lux_at: a scene with no lights reads 0 lux, direct and total", "[engine][lux]") {
    const Scene scene(closed_room());
    const LuxReading reading =
        lux_at(scene, {3.0, 2.0, 0.0}, {0.0, 0.0, 1.0}, LuxOptions{.samples = 128, .seed = 1});
    CHECK(reading.direct_lux.value == 0.0);
    CHECK(reading.direct_lux.standard_error == 0.0);
    CHECK(reading.total_lux.value == 0.0);
    CHECK(reading.total_lux.standard_error == 0.0);
}

TEST_CASE("lux_at: direct_lux is the direct part of lux_at", "[engine][lux]") {
    const Scene scene(room_with_ceiling_spot());
    for (const dvec3 p : {dvec3(3.0, 2.0, 0.0), dvec3(2.5, 1.0, 0.0), dvec3(6.0, 2.0, 1.5)}) {
        const dvec3 normal = p.x == 6.0 ? dvec3(-1.0, 0.0, 0.0) : dvec3(0.0, 0.0, 1.0);
        const LuxReading reading = lux_at(scene, p, normal, LuxOptions{.samples = 64, .seed = 2});
        INFO("at (" << p.x << ", " << p.y << ", " << p.z << ")");
        CHECK_THAT(reading.direct_lux.value, WithinRel(direct_lux(scene, p, normal), 1e-12));
    }
}

// ---- The owner's comment: a spot in the ceiling's plane ----------------------------------------

TEST_CASE("lux_at: a spot in the ceiling's plane lights the floor below it with "
          "I_peak / 3.5^2, the ceiling present",
          "[engine][lux][accuracy]") {
    // The owner's comment on #68: the spot at (3, 2, 3.5) in a closed 6 x 4 x 3.5 m room, aimed
    // straight down. The floor below it is on its axis, 3.5 m away: E = I_peak / 3.5^2. A shadow
    // ray that ran all the way to the light would end on the ceiling and read 0.
    const world::World w = room_with_ceiling_spot();
    const Scene scene(w);
    const double peak_cd = w.lights.front().spot->peak_intensity_cd();
    const double expected = peak_cd / (3.5 * 3.5);

    CHECK(light_visible(scene, {3.0, 2.0, 0.0}, {0.0, 0.0, 1.0}, {3.0, 2.0, 3.5}));
    CHECK_THAT(direct_lux(scene, {3.0, 2.0, 0.0}, {0.0, 0.0, 1.0}),
               WithinRel(expected, issue_tolerance));

    const LuxReading reading =
        lux_at(scene, {3.0, 2.0, 0.0}, {0.0, 0.0, 1.0}, LuxOptions{.samples = 256, .seed = 1});
    check_reading(reading.direct_lux, expected);
    CHECK(reading.direct_lux.standard_error == 0.0);
}

TEST_CASE("light_visible: a light in the ceiling's plane is visible from every point of the "
          "floor and walls",
          "[engine][lux]") {
    // The case above is one ray; whether a ray that ends exactly in the ceiling's plane finds
    // the ceiling depends on how its end rounds to a float. Over a grid of 2,000 points on the
    // floor and the four walls, rays to lights at the skeleton's spot and in the middle of the
    // ceiling end on it at every slope; without the gap, some of them must hit it.
    const Scene scene(closed_room());
    int hidden = 0;
    int rays = 0;
    for (const dvec3 light_m : {dvec3(3.465, 1.097, 3.5), dvec3(3.0, 2.0, 3.5)}) {
        for (int i = 0; i < 20; ++i) {
            for (int j = 0; j < 10; ++j) {
                const double a = (i + 0.5) / 20.0;
                const double b = (j + 0.5) / 10.0;
                const std::array<std::pair<dvec3, dvec3>, 5> points{{
                    {{6.0 * a, 4.0 * b, 0.0}, {0.0, 0.0, 1.0}},  // floor
                    {{6.0 * a, 0.0, 3.5 * b}, {0.0, 1.0, 0.0}},  // wall at y = 0
                    {{6.0 * a, 4.0, 3.5 * b}, {0.0, -1.0, 0.0}}, // wall at y = 4
                    {{0.0, 4.0 * a, 3.5 * b}, {1.0, 0.0, 0.0}},  // wall at x = 0
                    {{6.0, 4.0 * a, 3.5 * b}, {-1.0, 0.0, 0.0}}, // wall at x = 6
                }};
                for (const auto& [point, side] : points) {
                    ++rays;
                    if (!light_visible(scene, point, side, light_m)) {
                        ++hidden;
                        UNSCOPED_INFO("hidden from (" << point.x << ", " << point.y << ", "
                                                      << point.z << ")");
                    }
                }
            }
        }
    }
    CHECK(rays == 2000);
    CHECK(hidden == 0);
}

// ---- The owner's comment: the skeleton's chart -------------------------------------------------

namespace {

// The skeleton world, built by hand as world::compile builds it from tests/scenes/skeleton/
// (numbers from its README and libs/world/tests/test_skeleton_world.cpp). The engine cannot read
// the document, so this is a stand-in; the check on the compiled document itself belongs in
// tests/integration/ (M1-34).
world::World skeleton_by_hand() {
    // Face A of wall A: the wall's centre line along y = -0.05 from x = 0 to 6, 0.1 m thick, so
    // the face is the room's wall at y = 0, facing +y, u running towards -x.
    const geom::FaceFrame face_a =
        geom::wall_face_frame({0.0, -0.05}, {6.0, -0.05}, 0.1, geom::WallFace::a);
    constexpr geom::FlatWork chart_work{.width_m = 0.73, .height_m = 0.92, .depth_m = 0.0};
    constexpr geom::WallPlacement chart_at{.u_m = 3.0, .v_m = 1.5, .stand_off_m = 0.04};
    // compile.hpp's default_frame: 6 cm members, 4 cm deep, 5 mm rebate, Lambert 0.1.
    constexpr geom::BoxFrame chart_frame{.work_width_m = 0.73,
                                         .work_height_m = 0.92,
                                         .member_width_m = 0.06,
                                         .depth_m = 0.04,
                                         .rebate_m = 0.005};

    world::World w;
    w.meshes = {geom::room_mesh(6.0, 4.0, 3.5), geom::painting_quad(face_a, chart_work, chart_at),
                geom::box_frame(chart_frame)};
    w.materials = {world::LambertMaterial{dvec3(world::default_wall_albedo)},
                   world::PaintingSurface{}, world::LambertMaterial{dvec3(0.1)}};

    world::Instance room;
    room.part = world::Part::room;
    room.mesh = 0;
    room.material = 0;
    // painting_quad gives world coordinates already; compile instead uses the work's own
    // coordinates with on_wall_transform, which puts the same corners in the same place.
    world::Instance painting;
    painting.part = world::Part::painting;
    painting.mesh = 1;
    painting.material = 1;
    world::Instance frame;
    frame.part = world::Part::frame;
    frame.mesh = 2;
    frame.material = 2;
    frame.transform = geom::on_wall_transform(face_a, chart_at);
    w.instances = {room, painting, frame};

    world::Exhibit exhibit;
    exhibit.work_size_m = {0.73, 0.92};
    exhibit.face_centre_m = {3.0, 0.04, 1.5};
    exhibit.face_u = {-1.0, 0.0, 0.0};
    exhibit.face_v = {0.0, 0.0, 1.0};
    exhibit.face_normal = {0.0, 1.0, 0.0};
    exhibit.visible = {.min_m = {-0.36, -0.455}, .max_m = {0.36, 0.455}};
    exhibit.painting_instance = 1;
    exhibit.frame_instance = 2;
    w.exhibits = {exhibit};

    // Spot 1 on track A at the ceiling's height, aimed at the centre of the chart's face.
    w.lights.push_back(spot_light({3.465, 1.097, 3.5}, exhibit.face_centre_m, skeleton_spot()));
    return w;
}

} // namespace

TEST_CASE("lux_at: for the skeleton, the direct light on the chart's centre is above "
          "0 and matches I cos / d^2 from the world's light and exhibit",
          "[engine][lux][accuracy]") {
    // The owner's comment on #68. The spot sits in the ceiling's plane, so this fails if the
    // ceiling shadows it. The frame does not: the ray to the light leaves the face at
    // (0.465, 1.057, 2.0) / 2.309 and reaches the frame's front (4 cm out) 7.6 cm higher, at
    // z = 1.576 m, well inside the sight opening, which ends at z = 1.955 m.
    const world::World w = skeleton_by_hand();
    const Scene scene(w);
    REQUIRE(w.lights.size() == 1);
    REQUIRE(w.exhibits.size() == 1);
    const world::Light& spot = w.lights.front();
    const world::Exhibit& chart = w.exhibits.front();

    const dvec3 to_light = spot.luminaire.position_m() - chart.face_centre_m;
    const double d = glm::length(to_light);
    const double cos_theta = glm::dot(chart.face_normal, to_light / d);
    const double intensity = spot.luminaire.intensity_cd_towards(chart.face_centre_m);
    const double expected = intensity * cos_theta / (d * d);

    // The README's numbers: from 2.309 m away, at cos 0.4577 to the work's normal; and more than
    // the 460 lux a spot spreading its flux evenly over its 25 degree field would give.
    CHECK_THAT(d, WithinAbs(2.309, 1e-3));
    CHECK_THAT(cos_theta, WithinAbs(0.4577, 1e-4));
    CHECK(expected > 460.0);

    const double direct = direct_lux(scene, chart.face_centre_m, chart.face_normal);
    CHECK(direct > 0.0);
    CHECK_THAT(direct, WithinRel(expected, issue_tolerance));

    const LuxReading reading = lux_at(scene, chart.face_centre_m, chart.face_normal,
                                      LuxOptions{.samples = 256, .seed = 1});
    CHECK(reading.direct_lux.value > 0.0);
    check_reading(reading.direct_lux, expected);
    // The room reflects light onto the chart as well.
    CHECK(reading.total_lux.value > reading.direct_lux.value);
}

// ---- Indirect illuminance ----------------------------------------------------------------------

TEST_CASE("lux_at: in a closed room the total adds indirect light, with a finite positive "
          "standard error",
          "[engine][lux]") {
    const Scene scene(room_with_ceiling_spot());
    const LuxReading reading =
        lux_at(scene, {3.0, 2.0, 0.0}, {0.0, 0.0, 1.0}, LuxOptions{.samples = 1024, .seed = 1});
    const double indirect = reading.total_lux.value - reading.direct_lux.value;
    INFO("direct " << reading.direct_lux.value << ", total " << reading.total_lux.value << " +- "
                   << reading.total_lux.standard_error);

    CHECK(reading.total_lux.value > reading.direct_lux.value);
    CHECK(std::isfinite(reading.total_lux.standard_error));
    CHECK(reading.total_lux.standard_error > 0.0);

    // A sanity bound, not an accuracy check (the integrating sphere below is that): the
    // integrating-sphere estimate of the indirect illuminance in a room, rho Phi / (A (1 - rho)) =
    // 0.8 x 800 / (118 x 0.2) = 27.1 lux, is an average over the room's surfaces, and a point on
    // a box room's floor that does not see the floor's bright patch differs from it, but by far
    // less than a factor of 4 either way.
    const double sphere_estimate = 0.8 * 800.0 / (118.0 * 0.2);
    CHECK(indirect > sphere_estimate / 4.0);
    CHECK(indirect < sphere_estimate * 4.0);
}

namespace {

// The integrating sphere. In a sphere every patch of Lambertian wall irradiates every point of
// the wall equally (the form factor cos cos / (pi r^2) is 1 / (4 pi R^2) everywhere), so the
// light that has bounced at least once is the same everywhere. With a source of Phi lumens:
// E_ind = rho (Phi / A + E_ind), so E_ind = rho Phi / ((1 - rho) A), per colour channel.
//
// The sphere here is a polyhedron of 96 x 48 flat facets, so A is the mesh's area, and its light
// is uniform only nearly. Across a facet, at most 2.7 degrees from its centre, the direct light
// changes by about 3 (1 - cos 2.7 deg) = 0.3%. Wherever the radiance of an enclosure is uniform,
// the irradiance it gives is pi times it, whatever the shape, so the indirect light at one facet
// differs from the average by about that 0.3%. That holds for a source at the centre only: an
// off-centre source lights the tilted facets unevenly, by up to 2% (physics review of #68).
// The checks allow the 0.3% plus 3 standard errors, inside ADR 0002's 1%.
constexpr double sphere_intensity_cd = 1000.0;
constexpr double sphere_flux_lm = 4.0 * pi * sphere_intensity_cd;
constexpr double sphere_tolerance = 0.003;

struct SphereReading {
    double area_m2 = 0.0;
    double direct = 0.0;
    double direct_expected = 0.0;
    double indirect = 0.0;
    double standard_error = 0.0;
};

// The sphere of radius 1 m, of one material, lit by the isotropic 1000 cd source at its centre,
// read at the centroid of the facet nearest the equator at longitude 0, facing in.
SphereReading read_sphere(const world::Material& material, LuxOptions options) {
    const geom::Mesh sphere = faceted_sphere(1.0, 96, 48);
    REQUIRE(geom::is_closed(sphere));

    world::World w;
    add_lambert(w, sphere, 0.5);
    w.materials.front() = material;
    w.lights.push_back(isotropic_light({0.0, 0.0, 0.0}, sphere_intensity_cd));
    const Scene scene(w);

    const auto centroid = [&](std::size_t t) {
        return (sphere.positions[sphere.indices[3 * t]] +
                sphere.positions[sphere.indices[3 * t + 1]] +
                sphere.positions[sphere.indices[3 * t + 2]]) /
               3.0;
    };
    std::size_t best = 0;
    for (std::size_t t = 1; t < sphere.indices.size() / 3; ++t) {
        if (centroid(t).x > centroid(best).x) {
            best = t;
        }
    }
    const dvec3 point = centroid(best);
    const dvec3 normal = sphere.normals[sphere.indices[3 * best]];

    const LuxReading reading = lux_at(scene, point, normal, options);
    const double d = glm::length(point);
    return {.area_m2 = geom::area(sphere),
            .direct = reading.direct_lux.value,
            .direct_expected = sphere_intensity_cd * glm::dot(normal, -point / d) / (d * d),
            .indirect = reading.total_lux.value - reading.direct_lux.value,
            .standard_error = reading.total_lux.standard_error};
}

void check_indirect(const SphereReading& r, double expected) {
    INFO("indirect " << r.indirect << " +- " << r.standard_error << ", expected " << expected);
    CHECK(r.standard_error < 0.005 * expected); // enough samples for the check to mean something
    CHECK(std::abs(r.indirect - expected) <= sphere_tolerance * expected + 3.0 * r.standard_error);
}

} // namespace

TEST_CASE("lux_at: inside a closed sphere, the indirect illuminance is rho Phi / (A "
          "(1 - rho))",
          "[engine][lux][accuracy]") {
    // rho = 0.5: E_ind = Phi / A, about 1000 lux.
    constexpr double rho = 0.5;
    const SphereReading r =
        read_sphere(world::LambertMaterial{dvec3(rho)}, {.samples = 65536, .seed = 1});
    CHECK_THAT(r.area_m2, WithinRel(4.0 * pi, 0.01)); // the polyhedron is close to the sphere
    CHECK_THAT(r.direct, WithinRel(r.direct_expected, 1e-4));
    CHECK(r.standard_error > 0.0);
    check_indirect(r, rho * sphere_flux_lm / ((1.0 - rho) * r.area_m2));
}

TEST_CASE("lux_at: inside a closed sphere, the first bounce alone is rho Phi / A",
          "[engine][lux][accuracy]") {
    // With max_bounces 0 each path takes the direct light of the one surface it meets: rho Phi /
    // A, with no Monte Carlo spread in a sphere lit from its centre (every facet reflects the
    // same radiance, but for the 0.3% across facets). It pins the factor pi and the next-event
    // estimate without statistical slack.
    constexpr double rho = 0.5;
    const SphereReading r = read_sphere(world::LambertMaterial{dvec3(rho)},
                                        {.samples = 1024, .seed = 1, .max_bounces = 0});
    const double expected = rho * sphere_flux_lm / r.area_m2;
    INFO("indirect " << r.indirect << ", expected " << expected);
    CHECK_THAT(r.indirect, WithinRel(expected, sphere_tolerance));
}

TEST_CASE("lux_at: inside a closed sphere of albedo 0.9, Russian roulette still counts all the "
          "light",
          "[engine][lux][accuracy]") {
    // E_ind = 9 Phi / A: 90% of it has bounced more than once, and 0.9^65 = 0.1% of it more than
    // 64 times, which the old cap of 64 bounces lost (physics review of #68).
    constexpr double rho = 0.9;
    const SphereReading r =
        read_sphere(world::LambertMaterial{dvec3(rho)}, {.samples = 65536, .seed = 2});
    check_indirect(r, rho * sphere_flux_lm / ((1.0 - rho) * r.area_m2));
}

TEST_CASE("lux_at: inside a closed coloured sphere, the indirect light is the luminance of each "
          "channel's",
          "[engine][lux][accuracy]") {
    // Each channel c reflects rho_c / (1 - rho_c) of its share of the light, and the light's
    // colour (3000 K) has luminance 1, so E_ind = luminance(colour * rho / (1 - rho)) Phi / A.
    const dvec3 rho(0.9, 0.5, 0.2);
    const dvec3 rgb = cs::cct_to_linear_rgb(3000.0);
    const dvec3 colour = rgb / cs::luminance(rgb);
    const SphereReading r = read_sphere(world::LambertMaterial{rho}, {.samples = 65536, .seed = 3});
    check_indirect(r, cs::luminance(colour * rho / (1.0 - rho)) * sphere_flux_lm / r.area_m2);
}

TEST_CASE("lux_at: a painting's surface reflects with its base colour until texels are read",
          "[engine][lux][accuracy]") {
    // PaintingSurface's default base colour is 0.8 grey and it has no image: E_ind = 4 Phi / A.
    const world::PaintingSurface surface{};
    REQUIRE(surface.parameters.base.color == dvec3(0.8));
    const SphereReading r = read_sphere(surface, {.samples = 65536, .seed = 4});
    check_indirect(r, 0.8 * sphere_flux_lm / (0.2 * r.area_m2));
}

// ---- Seeds, samples and threads ----------------------------------------------------------------

TEST_CASE("lux_at: results are bit-identical with 1 and 8 threads for the same seed",
          "[engine][lux]") {
    // The issue's criterion. Samples run in chunks of 64 added in chunk order (lux.hpp), so the
    // thread count cannot change a bit; sample counts below, at and above one chunk, and many
    // chunks, more than the 8 threads.
    const Scene scene(room_with_ceiling_spot());
    cs::ThreadPool one(1);
    cs::ThreadPool eight(8);

    const std::uint32_t samples = GENERATE(1u, 63u, 64u, 65u, 1000u);
    INFO(samples << " samples");
    for (const auto& [point, normal] :
         {std::pair{dvec3(3.0, 2.0, 0.0), dvec3(0.0, 0.0, 1.0)},
          std::pair{dvec3(0.0, 1.5, 1.2), dvec3(1.0, 0.0, 0.0)},
          std::pair{dvec3(2.0, 3.0, 1.0), glm::normalize(dvec3(0.3, -1.0, 0.2))}}) {
        LuxOptions options{.samples = samples, .seed = 42};
        options.pool = &one;
        const LuxReading a = lux_at(scene, point, normal, options);
        options.pool = &eight;
        const LuxReading b = lux_at(scene, point, normal, options);
        options.pool = nullptr; // on the calling thread alone
        const LuxReading c = lux_at(scene, point, normal, options);

        CHECK(a.direct_lux.value == b.direct_lux.value);
        CHECK(a.direct_lux.standard_error == b.direct_lux.standard_error);
        CHECK(a.total_lux.value == b.total_lux.value);
        CHECK(a.total_lux.standard_error == b.total_lux.standard_error);

        CHECK(c.total_lux.value == a.total_lux.value);
        CHECK(c.total_lux.standard_error == a.total_lux.standard_error);
    }
}

TEST_CASE("lux_at: the same seed gives the same reading, another seed another indirect estimate",
          "[engine][lux]") {
    const Scene scene(room_with_ceiling_spot());
    cs::ThreadPool pool(4);
    const dvec3 point(3.0, 2.0, 0.0);
    const dvec3 up(0.0, 0.0, 1.0);

    const LuxReading a = lux_at(scene, point, up, {.samples = 256, .seed = 1, .pool = &pool});
    const LuxReading again = lux_at(scene, point, up, {.samples = 256, .seed = 1, .pool = &pool});
    const LuxReading b = lux_at(scene, point, up, {.samples = 256, .seed = 2, .pool = &pool});

    CHECK(again.total_lux.value == a.total_lux.value);
    CHECK(again.total_lux.standard_error == a.total_lux.standard_error);
    // The direct part is exact, so the seed does not touch it.
    CHECK(b.direct_lux.value == a.direct_lux.value);
    // The indirect estimate is random: with 256 samples of a continuous estimator, two seeds
    // agree to the last bit only by an astronomically unlikely chance.
    CHECK(b.total_lux.value != a.total_lux.value);
}

TEST_CASE("lux_at: the standard error matches the spread of readings over seeds", "[engine][lux]") {
    // 32 readings of 128 samples each with seeds 100 to 131. Their spread (the sample standard
    // deviation) estimates the true standard error with a relative error of about
    // 1 / sqrt(2 x 31) = 13%; the reported standard errors are each good to about the same. A
    // ratio between 0.5 and 2 is then 4 such errors away on the low side, more on the high: a
    // standard error that is wrong by a factor of 2 (a missing square root, sigma for sigma /
    // sqrt(n)) fails, an honest one passes. The room's estimator has light tails: the light lies
    // in the ceiling's plane, at least 2 m from every wall and 3.5 m from the floor.
    const Scene scene(room_with_ceiling_spot());
    cs::ThreadPool pool(4);
    std::vector<double> totals;
    std::vector<double> errors;
    for (std::uint64_t seed = 100; seed < 132; ++seed) {
        const LuxReading r = lux_at(scene, {3.0, 2.0, 0.0}, {0.0, 0.0, 1.0},
                                    {.samples = 128, .seed = seed, .pool = &pool});
        totals.push_back(r.total_lux.value);
        errors.push_back(r.total_lux.standard_error);
    }
    const double mean = sample_mean(totals);
    double squares = 0.0;
    for (const double t : totals) {
        squares += (t - mean) * (t - mean);
    }
    const double spread = std::sqrt(squares / static_cast<double>(totals.size() - 1));
    double error_squares = 0.0;
    for (const double e : errors) {
        error_squares += e * e;
    }
    const double reported = std::sqrt(error_squares / static_cast<double>(errors.size()));
    INFO("spread " << spread << ", reported " << reported);
    REQUIRE(reported > 0.0);
    CHECK(spread / reported > 0.5);
    CHECK(spread / reported < 2.0);
}

TEST_CASE("lux_at: more samples give a smaller standard error", "[engine][lux]") {
    // The standard error falls as 1 / sqrt(samples): 16 times the samples, a quarter of the
    // error. Asking for less than half leaves room for the noise in two estimates of it.
    const Scene scene(room_with_ceiling_spot());
    cs::ThreadPool pool(4);
    const LuxReading few =
        lux_at(scene, {3.0, 2.0, 0.0}, {0.0, 0.0, 1.0}, {.samples = 64, .seed = 9, .pool = &pool});
    const LuxReading many = lux_at(scene, {3.0, 2.0, 0.0}, {0.0, 0.0, 1.0},
                                   {.samples = 1024, .seed = 9, .pool = &pool});
    INFO("64 samples: +- " << few.total_lux.standard_error << ", 1024 samples: +- "
                           << many.total_lux.standard_error);
    CHECK(many.total_lux.standard_error < 0.5 * few.total_lux.standard_error);
}

TEST_CASE("lux_at: with one sample, the total's standard error is infinite", "[engine][lux]") {
    const Scene scene(room_with_ceiling_spot());
    const LuxReading reading =
        lux_at(scene, {3.0, 2.0, 0.0}, {0.0, 0.0, 1.0}, {.samples = 1, .seed = 1});
    CHECK(std::isinf(reading.total_lux.standard_error));
    CHECK(reading.total_lux.standard_error > 0.0);
    CHECK(reading.direct_lux.standard_error == 0.0);
    CHECK(std::isfinite(reading.total_lux.value));
}

// ---- Input -------------------------------------------------------------------------------------

TEST_CASE("lux_at: the normal is normalised", "[engine][lux]") {
    // (0, 0, 5) normalises to (0, 0, 1) exactly, so the readings are the same.
    const Scene scene(room_with_ceiling_spot());
    const LuxReading unit =
        lux_at(scene, {3.0, 2.0, 0.0}, {0.0, 0.0, 1.0}, {.samples = 128, .seed = 4});
    const LuxReading longer =
        lux_at(scene, {3.0, 2.0, 0.0}, {0.0, 0.0, 5.0}, {.samples = 128, .seed = 4});
    CHECK_THAT(longer.direct_lux.value, WithinRel(unit.direct_lux.value, 1e-12));
    CHECK_THAT(longer.total_lux.value, WithinRel(unit.total_lux.value, 1e-12));

    // And a short one, tilted: the same as its unit vector.
    const dvec3 tilted(0.001, 0.0, 0.001);
    CHECK_THAT(lux_at(scene, {3.0, 2.0, 0.0}, tilted, {.samples = 64, .seed = 4}).direct_lux.value,
               WithinRel(direct_lux(scene, {3.0, 2.0, 0.0}, glm::normalize(tilted)), 1e-9));
}

TEST_CASE("lux_at: a zero or non-finite normal, a non-finite point or 0 samples throws "
          "std::invalid_argument",
          "[engine][lux]") {
    const Scene scene(room_with_ceiling_spot());
    const dvec3 point(3.0, 2.0, 0.0);
    const dvec3 up(0.0, 0.0, 1.0);
    const LuxOptions options{.samples = 16, .seed = 1};

    CHECK_THROWS_AS(lux_at(scene, point, dvec3(0.0), options), std::invalid_argument);
    CHECK_THROWS_AS(lux_at(scene, point, {not_a_number, 0.0, 1.0}, options), std::invalid_argument);
    CHECK_THROWS_AS(lux_at(scene, point, {0.0, inf, 1.0}, options), std::invalid_argument);
    CHECK_THROWS_AS(lux_at(scene, point, {0.0, 0.0, -inf}, options), std::invalid_argument);
    CHECK_THROWS_AS(lux_at(scene, {not_a_number, 2.0, 0.0}, up, options), std::invalid_argument);
    CHECK_THROWS_AS(lux_at(scene, {3.0, inf, 0.0}, up, options), std::invalid_argument);
    CHECK_THROWS_AS(lux_at(scene, {3.0, 2.0, -inf}, up, options), std::invalid_argument);
    CHECK_THROWS_AS(lux_at(scene, point, up, {.samples = 0, .seed = 1}), std::invalid_argument);
}
