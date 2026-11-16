// SPDX-License-Identifier: Apache-2.0

// The lux on a work (work_lux.hpp), from issue #81 (M1-17) and the owner's comment of 2026-11-12:
// work_lux_grid reads the lux meter at 5 x 5 points at the centres of a 5 x 5 division of the
// exhibit's visible area (the frame's sight opening, or the whole work when unframed), never on
// its edge, and summarises direct and total illuminance with mean, minimum, maximum and standard
// error. Checked here: where the points lie and in what order, the direct light at each against
// I cos / d^2, the summary against the points, the seeds and the thread count, and invalid input.
//
// The skeleton scene's grid, compiled from the document, is checked in
// tests/integration/test_skeleton_work_lux.cpp: the engine's tests cannot load a project.
//
// Written from the issue and the public headers only (test-author), without the implementation.

#include <cs/engine/lux.hpp>
#include <cs/engine/scene.hpp>
#include <cs/engine/work_lux.hpp>

#include <cs/core/random.hpp>
#include <cs/core/thread_pool.hpp>
#include <cs/geom/mesh.hpp>
#include <cs/light/distribution.hpp>
#include <cs/light/luminaire.hpp>
#include <cs/world/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using cs::engine::lux_at;
using cs::engine::LuxOptions;
using cs::engine::LuxReading;
using cs::engine::Scene;
using cs::engine::work_lux_grid;
using cs::engine::work_lux_points;
using cs::engine::WorkLux;
using glm::dvec2;
using glm::dvec3;

namespace geom = cs::geom;
namespace light = cs::light;
namespace world = cs::world;

namespace {

constexpr double pi = std::numbers::pi;
constexpr double not_a_number = std::numeric_limits<double>::quiet_NaN();
constexpr double inf = std::numeric_limits<double>::infinity();

// Point positions are a handful of additions and multiplications of numbers below 2 m: double
// rounding leaves them good to about 1e-15 m. 1e-12 m allows that and catches any real error.
constexpr double position_tolerance_m = 1e-12;

// ---- Lights (as in test_lux.cpp) ---------------------------------------------------------------

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

// An isotropic light. World::Light also carries the distribution as a generic spot for the
// shaders: a uniform cone over the whole sphere, whose intensity is 4 pi I / (4 pi) = I.
world::Light isotropic_light(dvec3 position_m, double intensity_cd) {
    return world::Light{
        .entity = world::Id{},
        .luminaire = light::Luminaire(position_m, position_m + dvec3(0.0, 0.0, -1.0),
                                      std::make_shared<const Isotropic>(intensity_cd)),
        .spot =
            std::make_shared<const light::GenericSpot>(4.0 * pi * intensity_cd, 2.0 * pi, 2.0 * pi),
        .colour_temperature_k = 3000.0};
}

// ---- Meshes and worlds -------------------------------------------------------------------------

// A rectangle centred on `centre`, spanning +-half_u and +-half_v, facing cross(half_u, half_v):
// four vertices, two triangles wound counter-clockwise seen from its front.
geom::Mesh rectangle(dvec3 centre, dvec3 half_u, dvec3 half_v) {
    const dvec3 normal = glm::normalize(glm::cross(half_u, half_v));
    geom::Mesh mesh;
    mesh.positions = {centre - half_u - half_v, centre + half_u - half_v, centre + half_u + half_v,
                      centre - half_u + half_v};
    mesh.normals.assign(4, normal);
    mesh.uvs = {{0.0, 1.0}, {1.0, 1.0}, {1.0, 0.0}, {0.0, 0.0}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

// The work used throughout: its face centred at (0, 0, 1.5), u along +x, v up, so its normal is
// u x v = (1, 0, 0) x (0, 0, 1) = (0, -1, 0), facing -y. 0.8 m wide and 0.6 m high.
const dvec3 face_centre(0.0, 0.0, 1.5);
const dvec3 face_u(1.0, 0.0, 0.0);
const dvec3 face_v(0.0, 0.0, 1.0);
const dvec3 face_normal(0.0, -1.0, 0.0);
constexpr double work_width_m = 0.8;
constexpr double work_height_m = 0.6;

world::Exhibit exhibit_with(world::FaceRect visible, std::uint32_t painting_instance,
                            std::optional<std::uint32_t> frame_instance) {
    world::Exhibit e;
    e.work_size_m = {work_width_m, work_height_m};
    e.face_centre_m = face_centre;
    e.face_u = face_u;
    e.face_v = face_v;
    e.face_normal = face_normal;
    e.visible = visible;
    e.painting_instance = painting_instance;
    e.frame_instance = frame_instance;
    return e;
}

// Unframed: the visible area is the whole work.
world::Exhibit unframed_exhibit() {
    return exhibit_with({.min_m = {-0.4, -0.3}, .max_m = {0.4, 0.3}}, 1, std::nullopt);
}

// Framed with a 5 mm rebate (geom::sight_opening: the work less twice the rebate): visible
// 0.79 x 0.59 m.
world::Exhibit framed_exhibit() {
    return exhibit_with({.min_m = {-0.395, -0.295}, .max_m = {0.395, 0.295}}, 1, 2u);
}

// The isotropic 1000 cd light, in front of the work (y < 0), above and to its right.
const dvec3 light_position(0.3, -1.0, 2.5);
constexpr double light_cd = 1000.0;

// A floor at z = 0, 1000 m across, of albedo 0.5; the work's face as a painting; one light. The
// floor is instance 0, the painting instance 1, so that painting_instance is not 0 and the seed
// test below can tell it from a constant. The floor reflects the light back onto the work, so the
// total illuminance has an indirect part with a standard error above 0.
world::World work_world() {
    world::World w;
    w.meshes = {rectangle(dvec3(0.0), {500.0, 0.0, 0.0}, {0.0, 500.0, 0.0}),
                rectangle(face_centre, 0.5 * work_width_m * face_u, 0.5 * work_height_m * face_v)};
    w.materials = {world::LambertMaterial{dvec3(0.5)}, world::PaintingSurface{}};
    world::Instance floor;
    floor.part = world::Part::room;
    floor.mesh = 0;
    floor.material = 0;
    world::Instance painting;
    painting.part = world::Part::painting;
    painting.mesh = 1;
    painting.material = 1;
    w.instances = {floor, painting};
    w.exhibits = {unframed_exhibit()};
    w.lights = {isotropic_light(light_position, light_cd)};
    return w;
}

// The grid as work_lux.hpp specifies it, written out: point k in column k % 5 from the left
// (along face_u) and row k / 5 from the top (against face_v), at the centre of its cell of the
// visible area.
dvec2 expected_point(const world::FaceRect& visible, std::size_t k) {
    const double width = visible.max_m.x - visible.min_m.x;
    const double height = visible.max_m.y - visible.min_m.y;
    const double column = static_cast<double>(k % 5);
    const double row = static_cast<double>(k / 5);
    return {visible.min_m.x + (column + 0.5) * width / 5.0,
            visible.max_m.y - (row + 0.5) * height / 5.0};
}

// I cos / d^2 for the isotropic light, at a world point on the face, facing along its normal:
// d = |L - p|, cos = n . (L - p) / d.
double analytic_direct_lux(dvec3 point_m) {
    const dvec3 to_light = light_position - point_m;
    const double d = glm::length(to_light);
    const double cos_theta = glm::dot(face_normal, to_light / d);
    return light_cd * cos_theta / (d * d);
}

void check_points(const world::Exhibit& exhibit) {
    const auto points = work_lux_points(exhibit);
    REQUIRE(points.size() == 25);
    for (std::size_t k = 0; k < points.size(); ++k) {
        const dvec2 expected = expected_point(exhibit.visible, k);
        INFO("point " << k << ": (" << points[k].x << ", " << points[k].y << "), expected ("
                      << expected.x << ", " << expected.y << ")");
        CHECK_THAT(points[k].x, WithinAbs(expected.x, position_tolerance_m));
        CHECK_THAT(points[k].y, WithinAbs(expected.y, position_tolerance_m));
    }
}

bool same_bits(const cs::engine::Estimate& a, const cs::engine::Estimate& b) {
    return std::bit_cast<std::uint64_t>(a.value) == std::bit_cast<std::uint64_t>(b.value) &&
           std::bit_cast<std::uint64_t>(a.standard_error) ==
               std::bit_cast<std::uint64_t>(b.standard_error);
}

bool same_bits(const cs::engine::LuxSummary& a, const cs::engine::LuxSummary& b) {
    return std::bit_cast<std::uint64_t>(a.mean) == std::bit_cast<std::uint64_t>(b.mean) &&
           std::bit_cast<std::uint64_t>(a.minimum) == std::bit_cast<std::uint64_t>(b.minimum) &&
           std::bit_cast<std::uint64_t>(a.maximum) == std::bit_cast<std::uint64_t>(b.maximum) &&
           std::bit_cast<std::uint64_t>(a.standard_error) ==
               std::bit_cast<std::uint64_t>(b.standard_error);
}

bool same_bits(const WorkLux& a, const WorkLux& b) {
    for (std::size_t k = 0; k < a.points.size(); ++k) {
        if (!same_bits(a.points[k].reading.direct_lux, b.points[k].reading.direct_lux) ||
            !same_bits(a.points[k].reading.total_lux, b.points[k].reading.total_lux)) {
            UNSCOPED_INFO("point " << k << " differs: total " << a.points[k].reading.total_lux.value
                                   << " and " << b.points[k].reading.total_lux.value);
            return false;
        }
    }
    return same_bits(a.direct_lux, b.direct_lux) && same_bits(a.total_lux, b.total_lux);
}

} // namespace

// ---- Where the points lie ----------------------------------------------------------------------

TEST_CASE("work_lux_points: 25 points at the centres of a 5 x 5 division of the visible area, "
          "row 0 at the top and column 0 at the left",
          "[engine][work_lux]") {
    static_assert(cs::engine::work_lux_grid_side == 5);
    static_assert(cs::engine::work_lux_grid_points == 25);

    SECTION("unframed: the visible area is the whole work") {
        check_points(unframed_exhibit());
    }
    SECTION("framed: the visible area is the sight opening, smaller than the work") {
        check_points(framed_exhibit());
    }
    SECTION("a visible area off the face's centre") {
        // Nothing requires the area to be centred on face_centre_m; the grid follows the area.
        check_points(exhibit_with({.min_m = {-0.25, -0.1}, .max_m = {0.35, 0.4}}, 1, 2u));
    }
}

TEST_CASE("work_lux_points: the unframed work's points, one by one", "[engine][work_lux]") {
    // 0.8 x 0.6 m: cells 0.16 x 0.12 m, so the columns are at -0.32, -0.16, 0, 0.16, 0.32 and the
    // rows at 0.24, 0.12, 0, -0.12, -0.24 from the top down.
    const auto p = work_lux_points(unframed_exhibit());
    const std::array<double, 5> columns{-0.32, -0.16, 0.0, 0.16, 0.32};
    const std::array<double, 5> rows{0.24, 0.12, 0.0, -0.12, -0.24};
    for (std::size_t k = 0; k < 25; ++k) {
        INFO("point " << k);
        CHECK_THAT(p[k].x, WithinAbs(columns[k % 5], position_tolerance_m));
        CHECK_THAT(p[k].y, WithinAbs(rows[k / 5], position_tolerance_m));
    }
    // Point 12 is the centre of the work's face.
    CHECK_THAT(p[12].x, WithinAbs(0.0, position_tolerance_m));
    CHECK_THAT(p[12].y, WithinAbs(0.0, position_tolerance_m));
}

TEST_CASE("work_lux_points: no point lies on the edge of the visible area; the outer ones are a "
          "tenth of its width and height in",
          "[engine][work_lux]") {
    for (const world::Exhibit& e :
         {unframed_exhibit(), framed_exhibit(),
          exhibit_with({.min_m = {-0.25, -0.1}, .max_m = {0.35, 0.4}}, 1, 2u)}) {
        const world::FaceRect& v = e.visible;
        const double width = v.max_m.x - v.min_m.x;
        const double height = v.max_m.y - v.min_m.y;
        INFO("visible (" << v.min_m.x << ", " << v.min_m.y << ") to (" << v.max_m.x << ", "
                         << v.max_m.y << ")");
        const auto p = work_lux_points(e);
        for (std::size_t k = 0; k < 25; ++k) {
            INFO("point " << k);
            CHECK(p[k].x > v.min_m.x);
            CHECK(p[k].x < v.max_m.x);
            CHECK(p[k].y > v.min_m.y);
            CHECK(p[k].y < v.max_m.y);
        }
        // Column 0 (points 0, 5, ..., 20) a tenth of the width from the left edge, column 4 from
        // the right; row 0 (points 0 to 4) a tenth of the height below the top, row 4 above the
        // bottom.
        for (std::size_t i = 0; i < 5; ++i) {
            INFO("row or column " << i);
            CHECK_THAT(p[5 * i].x - v.min_m.x, WithinAbs(0.1 * width, position_tolerance_m));
            CHECK_THAT(v.max_m.x - p[5 * i + 4].x, WithinAbs(0.1 * width, position_tolerance_m));
            CHECK_THAT(v.max_m.y - p[i].y, WithinAbs(0.1 * height, position_tolerance_m));
            CHECK_THAT(p[20 + i].y - v.min_m.y, WithinAbs(0.1 * height, position_tolerance_m));
        }
    }
}

TEST_CASE("work_lux_points: along a row u grows to the right, down a column v falls",
          "[engine][work_lux]") {
    // The visitor's reading order: point 1 is right of point 0 along face_u, point 5 below it.
    const auto p = work_lux_points(framed_exhibit());
    for (std::size_t row = 0; row < 5; ++row) {
        for (std::size_t column = 0; column + 1 < 5; ++column) {
            CHECK(p[5 * row + column + 1].x > p[5 * row + column].x);
            CHECK(p[5 * row + column + 1].y == p[5 * row + column].y);
        }
    }
    for (std::size_t column = 0; column < 5; ++column) {
        for (std::size_t row = 0; row + 1 < 5; ++row) {
            CHECK(p[5 * (row + 1) + column].y < p[5 * row + column].y);
            CHECK(p[5 * (row + 1) + column].x == p[5 * row + column].x);
        }
    }
}

// ---- The readings ------------------------------------------------------------------------------

TEST_CASE("work_lux_grid: each point's direct illuminance is I cos / d^2 at that point",
          "[engine][work_lux][accuracy]") {
    // Nothing stands between the light and the face, so each point's direct light is the
    // isotropic 1000 cd's I cos / d^2 exactly. It is a sum in double, not an estimate (lux.hpp),
    // so 1e-6 relative holds it far tighter than the issue's 1% while leaving room for rounding in
    // the point's position and the direction to the light. Here d runs from 1.26 to 1.71 m and the
    // readings from about 200 to 505 lux.
    const world::World w = work_world();
    const Scene scene(w);
    const world::Exhibit& exhibit = w.exhibits.front();
    const WorkLux grid = work_lux_grid(scene, exhibit, LuxOptions{.samples = 64, .seed = 1});

    for (std::size_t k = 0; k < 25; ++k) {
        const dvec2 face_m = expected_point(exhibit.visible, k);
        const dvec3 point_m = exhibit.face_point(face_m);
        const cs::engine::WorkLuxPoint& p = grid.points[k];
        INFO("point " << k << " at (" << point_m.x << ", " << point_m.y << ", " << point_m.z
                      << "): direct " << p.reading.direct_lux.value << ", expected "
                      << analytic_direct_lux(point_m));
        CHECK_THAT(p.face_m.x, WithinAbs(face_m.x, position_tolerance_m));
        CHECK_THAT(p.face_m.y, WithinAbs(face_m.y, position_tolerance_m));
        CHECK_THAT(p.point_m.x, WithinAbs(point_m.x, position_tolerance_m));
        CHECK_THAT(p.point_m.y, WithinAbs(point_m.y, position_tolerance_m));
        CHECK_THAT(p.point_m.z, WithinAbs(point_m.z, position_tolerance_m));
        CHECK(p.reading.direct_lux.standard_error == 0.0);
        CHECK_THAT(p.reading.direct_lux.value, WithinRel(analytic_direct_lux(point_m), 1e-6));
        // The floor reflects light onto the face: the total is more than the direct.
        CHECK(p.reading.total_lux.value > p.reading.direct_lux.value);
    }
    // Sanity of the analytic case: the points nearest the light read most.
    CHECK(analytic_direct_lux(exhibit.face_point(expected_point(exhibit.visible, 4))) >
          analytic_direct_lux(exhibit.face_point(expected_point(exhibit.visible, 20))));
}

TEST_CASE("work_lux_grid: the points match work_lux_points and Exhibit::face_point",
          "[engine][work_lux]") {
    const world::World w = work_world();
    const Scene scene(w);
    // The framed exhibit's smaller visible area; this world has no frame, so no frame instance.
    world::Exhibit framed = framed_exhibit();
    framed.frame_instance.reset();
    const auto points = work_lux_points(framed);
    const WorkLux grid = work_lux_grid(scene, framed, LuxOptions{.samples = 16, .seed = 3});
    for (std::size_t k = 0; k < 25; ++k) {
        INFO("point " << k);
        CHECK(grid.points[k].face_m == points[k]);
        const dvec3 world_point = framed.face_point(points[k]);
        CHECK_THAT(grid.points[k].point_m.x, WithinAbs(world_point.x, position_tolerance_m));
        CHECK_THAT(grid.points[k].point_m.y, WithinAbs(world_point.y, position_tolerance_m));
        CHECK_THAT(grid.points[k].point_m.z, WithinAbs(world_point.z, position_tolerance_m));
    }
}

TEST_CASE("work_lux_grid: point k is lux_at with the seed hash_seed(seed, k, painting_instance, "
          "0, work_lux_dimension)",
          "[engine][work_lux]") {
    // work_lux.hpp: each point "is measured with lux_at and the caller's options, except for its
    // seed". So its reading equals lux_at's at that point, facing along the face's normal, bit for
    // bit. painting_instance is 1 here, so a seed that leaves it out shows.
    const world::World w = work_world();
    const Scene scene(w);
    const world::Exhibit& exhibit = w.exhibits.front();
    REQUIRE(exhibit.painting_instance == 1);
    const LuxOptions options{.samples = 64, .seed = 17};
    const WorkLux grid = work_lux_grid(scene, exhibit, options);

    for (const std::uint32_t k : {0u, 7u, 12u, 24u}) {
        INFO("point " << k);
        LuxOptions own = options;
        own.seed = cs::hash_seed(options.seed, k, exhibit.painting_instance, 0,
                                 cs::engine::work_lux_dimension);
        const LuxReading expected = lux_at(scene, grid.points[k].point_m, exhibit.face_normal, own);
        CHECK(same_bits(grid.points[k].reading.direct_lux, expected.direct_lux));
        CHECK(same_bits(grid.points[k].reading.total_lux, expected.total_lux));
    }

    // So the 25 indirect estimates differ from one another: no two points share their numbers.
    // Two points in the same light could agree to the last bit only by an astronomically
    // unlikely chance.
    CHECK(grid.points[11].reading.total_lux.value != grid.points[13].reading.total_lux.value);
}

TEST_CASE("work_lux_grid: the summaries are the mean, minimum and maximum of the points, with "
          "the standard error sqrt(sum se^2) / 25",
          "[engine][work_lux]") {
    // Sums of 25 values in a different order differ by a few ulps: 1e-12 relative.
    const world::World w = work_world();
    const Scene scene(w);
    const WorkLux grid =
        work_lux_grid(scene, w.exhibits.front(), LuxOptions{.samples = 64, .seed = 5});

    double direct_sum = 0.0;
    double total_sum = 0.0;
    double total_squared_errors = 0.0;
    double direct_min = inf;
    double direct_max = -inf;
    double total_min = inf;
    double total_max = -inf;
    for (const cs::engine::WorkLuxPoint& p : grid.points) {
        const double direct = p.reading.direct_lux.value;
        const double total = p.reading.total_lux.value;
        direct_sum += direct;
        total_sum += total;
        total_squared_errors +=
            p.reading.total_lux.standard_error * p.reading.total_lux.standard_error;
        direct_min = std::min(direct_min, direct);
        direct_max = std::max(direct_max, direct);
        total_min = std::min(total_min, total);
        total_max = std::max(total_max, total);
    }

    CHECK_THAT(grid.direct_lux.mean, WithinRel(direct_sum / 25.0, 1e-12));
    CHECK(grid.direct_lux.minimum == direct_min);
    CHECK(grid.direct_lux.maximum == direct_max);
    CHECK(grid.direct_lux.standard_error == 0.0); // point lights: exact
    CHECK(direct_min < direct_max);               // the light is not even over the work

    CHECK_THAT(grid.total_lux.mean, WithinRel(total_sum / 25.0, 1e-12));
    CHECK(grid.total_lux.minimum == total_min);
    CHECK(grid.total_lux.maximum == total_max);
    REQUIRE(total_squared_errors > 0.0); // the floor's light is an estimate
    CHECK_THAT(grid.total_lux.standard_error,
               WithinRel(std::sqrt(total_squared_errors) / 25.0, 1e-12));
    CHECK(grid.total_lux.mean > grid.direct_lux.mean);
    CHECK(grid.direct_lux.minimum <= grid.direct_lux.mean);
    CHECK(grid.direct_lux.mean <= grid.direct_lux.maximum);
}

TEST_CASE("work_lux_grid: with no lights, every point and summary reads 0", "[engine][work_lux]") {
    world::World w = work_world();
    w.lights.clear();
    const Scene scene(w);
    const WorkLux grid =
        work_lux_grid(scene, w.exhibits.front(), LuxOptions{.samples = 16, .seed = 1});
    for (const cs::engine::WorkLuxPoint& p : grid.points) {
        CHECK(p.reading.direct_lux.value == 0.0);
        CHECK(p.reading.total_lux.value == 0.0);
    }
    for (const cs::engine::LuxSummary& s : {grid.direct_lux, grid.total_lux}) {
        CHECK(s.mean == 0.0);
        CHECK(s.minimum == 0.0);
        CHECK(s.maximum == 0.0);
        CHECK(s.standard_error == 0.0);
    }
}

// ---- Seeds and threads -------------------------------------------------------------------------

TEST_CASE("work_lux_grid: the same seed gives the same grid, bit for bit, with 1 or 8 threads or "
          "none; another seed another total",
          "[engine][work_lux]") {
    const world::World w = work_world();
    const Scene scene(w);
    const world::Exhibit& exhibit = w.exhibits.front();
    cs::ThreadPool one(1);
    cs::ThreadPool eight(8);

    LuxOptions options{.samples = 100, .seed = 42}; // not a whole number of 64-sample chunks
    const WorkLux alone = work_lux_grid(scene, exhibit, options);
    const WorkLux again = work_lux_grid(scene, exhibit, options);
    options.pool = &one;
    const WorkLux with_one = work_lux_grid(scene, exhibit, options);
    options.pool = &eight;
    const WorkLux with_eight = work_lux_grid(scene, exhibit, options);

    CHECK(same_bits(alone, again));
    CHECK(same_bits(alone, with_one));
    CHECK(same_bits(alone, with_eight));

    options.seed = 43;
    const WorkLux other = work_lux_grid(scene, exhibit, options);
    // The direct part is exact and does not depend on the seed; the indirect estimate does.
    CHECK(same_bits(other.direct_lux, alone.direct_lux));
    CHECK(other.total_lux.mean != alone.total_lux.mean);
}

// ---- Input -------------------------------------------------------------------------------------

TEST_CASE("work_lux_grid: an empty or non-finite visible area, a non-finite face or 0 samples "
          "throws std::invalid_argument",
          "[engine][work_lux]") {
    const world::World w = work_world();
    const Scene scene(w);
    const LuxOptions options{.samples = 16, .seed = 1};
    REQUIRE_NOTHROW(work_lux_grid(scene, unframed_exhibit(), options));

    struct Case {
        std::string why;
        world::Exhibit exhibit;
    };
    const auto with_visible = [](dvec2 min_m, dvec2 max_m) {
        return exhibit_with({.min_m = min_m, .max_m = max_m}, 1, std::nullopt);
    };
    std::vector<Case> cases = {
        {"an empty visible area (a point)", with_visible({0.1, 0.1}, {0.1, 0.1})},
        {"no width", with_visible({0.2, -0.3}, {0.2, 0.3})},
        {"no height", with_visible({-0.4, 0.0}, {0.4, 0.0})},
        {"min right of max", with_visible({0.4, -0.3}, {-0.4, 0.3})},
        {"min above max", with_visible({-0.4, 0.3}, {0.4, -0.3})},
        {"the default, all zero", with_visible({0.0, 0.0}, {0.0, 0.0})},
        {"a NaN corner", with_visible({not_a_number, -0.3}, {0.4, 0.3})},
        {"an infinite corner", with_visible({-0.4, -0.3}, {inf, 0.3})},
        {"an infinite area", with_visible({-inf, -inf}, {inf, inf})},
    };
    {
        world::Exhibit e = unframed_exhibit();
        e.face_centre_m.x = not_a_number;
        cases.push_back({"a NaN face centre", e});
    }
    {
        world::Exhibit e = unframed_exhibit();
        e.face_centre_m.z = inf;
        cases.push_back({"an infinite face centre", e});
    }
    {
        world::Exhibit e = unframed_exhibit();
        e.face_u.y = not_a_number;
        cases.push_back({"a NaN face_u", e});
    }
    {
        world::Exhibit e = unframed_exhibit();
        e.face_v.z = inf;
        cases.push_back({"an infinite face_v", e});
    }
    {
        world::Exhibit e = unframed_exhibit();
        e.face_normal = {0.0, -inf, 0.0};
        cases.push_back({"an infinite face normal", e});
    }

    for (const Case& c : cases) {
        INFO(c.why);
        CHECK_THROWS_AS(work_lux_grid(scene, c.exhibit, options), std::invalid_argument);
    }

    // As lux_at does for 0 samples.
    CHECK_THROWS_AS(work_lux_grid(scene, unframed_exhibit(), LuxOptions{.samples = 0, .seed = 1}),
                    std::invalid_argument);
}
