// SPDX-License-Identifier: Apache-2.0

// The painted face of a flat work hung on a wall (painting.hpp): a rectangle at the work's true
// size, centred at the OnWall placement, facing into the room, with image UVs.

#include "mesh_checks.hpp"

#include <cs/geom/painting.hpp>
#include <cs/geom/wall_face.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>

using namespace cs::geom;
using namespace cs::geom::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using glm::dvec2;
using glm::dvec3;
using glm::dvec4;

namespace {

constexpr double infinity = std::numeric_limits<double>::infinity();
constexpr double not_a_number = std::numeric_limits<double>::quiet_NaN();

// The issue's tolerance for the corners: 0.1 mm.
constexpr double corner_tolerance_m = 1e-4;

// Face A of the wall from (0, 0) to (6, 0), 0.1 m thick: on y = 0.05, facing +y, with u running
// along -x from its left end at x = 6 (wall_face.hpp).
FaceFrame face_a() {
    return wall_face_frame({0.0, 0.0}, {6.0, 0.0}, 0.1, WallFace::a);
}

// The work and placement of the acceptance criteria: 0.73 m wide, 0.92 m high, a print with no
// depth, centred at u = 3, v = 1.5, 0.04 m out from the wall.
constexpr FlatWork chart{.width_m = 0.73, .height_m = 0.92, .depth_m = 0.0};
constexpr WallPlacement chart_at{.u_m = 3.0, .v_m = 1.5, .stand_off_m = 0.04};

// The corners of the work's front in its own coordinates (wall_face.hpp: x right seen from the
// front, y up, z out of the front, origin at the centre of its back), in painting.hpp's order:
// bottom-left, bottom-right, top-right, top-left.
std::array<dvec3, 4> local_corners(const FlatWork& work) {
    const double x = work.width_m / 2.0;
    const double y = work.height_m / 2.0;
    const double z = work.depth_m;
    return {dvec3(-x, -y, z), dvec3(x, -y, z), dvec3(x, y, z), dvec3(-x, y, z)};
}

// Checks the quad's shape: four vertices and two triangles that meet along a diagonal of the
// rectangle (0-2 or 1-3), so that together they cover it, not overlap.
void check_two_triangles_on_a_diagonal(const Mesh& quad) {
    REQUIRE(quad.positions.size() == 4);
    REQUIRE(quad.indices.size() == 6);
    const std::set<std::uint32_t> first(quad.indices.begin(), quad.indices.begin() + 3);
    const std::set<std::uint32_t> second(quad.indices.begin() + 3, quad.indices.end());
    std::set<std::uint32_t> shared;
    for (const auto i : first) {
        if (second.count(i) != 0) {
            shared.insert(i);
        }
    }
    const bool diagonal =
        shared == std::set<std::uint32_t>{0, 2} || shared == std::set<std::uint32_t>{1, 3};
    CHECK(first.size() == 3);
    CHECK(second.size() == 3);
    CHECK(diagonal);
}

} // namespace

TEST_CASE("painting_quad: a 0.73 x 0.92 m work at u = 3, v = 1.5, stand-off 0.04 has its corners "
          "within 0.1 mm",
          "[geom][painting]") {
    // Seen from the front, from +y looking towards -y, right is -x: the left edge is at
    // x = 3 + 0.365 and the right edge at 3 - 0.365. The face is 0.05 + 0.04 = 0.09 m out on y;
    // the bottom is at 1.5 - 0.46 = 1.04 m and the top at 1.5 + 0.46 = 1.96 m.
    const Mesh quad = painting_quad(face_a(), chart, chart_at);
    REQUIRE(quad.positions.size() == 4);
    CHECK_THAT(quad.positions[0], VecWithin(dvec3(3.365, 0.09, 1.04), corner_tolerance_m));
    CHECK_THAT(quad.positions[1], VecWithin(dvec3(2.635, 0.09, 1.04), corner_tolerance_m));
    CHECK_THAT(quad.positions[2], VecWithin(dvec3(2.635, 0.09, 1.96), corner_tolerance_m));
    CHECK_THAT(quad.positions[3], VecWithin(dvec3(3.365, 0.09, 1.96), corner_tolerance_m));
}

TEST_CASE("painting_quad: the normal points into the room and the triangles wind with it",
          "[geom][painting]") {
    // The room is on face A's side, +y.
    const Mesh quad = painting_quad(face_a(), chart, chart_at);
    check_mesh_conventions(quad);
    REQUIRE(quad.normals.size() == 4);
    for (const dvec3& n : quad.normals) {
        CHECK_THAT(n, VecWithin(dvec3(0.0, 1.0, 0.0), unit_tolerance));
    }
}

TEST_CASE("painting_quad: four vertices and two triangles covering width x height",
          "[geom][painting]") {
    // 0.73 * 0.92 = 0.6716 m^2; only rounding (1e-16 relative) separates the mesh from it.
    const Mesh quad = painting_quad(face_a(), chart, chart_at);
    check_two_triangles_on_a_diagonal(quad);
    CHECK_THAT(area(quad), WithinRel(0.6716, 1e-9));
    // Two triangles with open edges: a quad is not a closed surface.
    CHECK_FALSE(is_closed(quad));
}

TEST_CASE("painting_quad: UVs are image coordinates, (0, 0) at the top-left", "[geom][painting]") {
    // painting.hpp: u to the right, v down, (0, 0) top-left and (1, 1) bottom-right. So in the
    // vertex order bottom-left, bottom-right, top-right, top-left: (0, 1), (1, 1), (1, 0), (0, 0).
    const Mesh quad = painting_quad(face_a(), chart, chart_at);
    check_mesh_conventions(quad);
    REQUIRE(quad.uvs.size() == 4);
    CHECK_THAT(quad.uvs[0], VecWithin(dvec2(0.0, 1.0), unit_tolerance));
    CHECK_THAT(quad.uvs[1], VecWithin(dvec2(1.0, 1.0), unit_tolerance));
    CHECK_THAT(quad.uvs[2], VecWithin(dvec2(1.0, 0.0), unit_tolerance));
    CHECK_THAT(quad.uvs[3], VecWithin(dvec2(0.0, 0.0), unit_tolerance));

    // Across each triangle, a step of 1 in u crosses the work's width to the right (face.u, -x
    // here), and a step of 1 in v its height downwards: dP/du = 0.73 face.u, dP/dv = -0.92 face.v.
    const FaceFrame face = face_a();
    for (std::size_t t = 0; t < triangle_count(quad); ++t) {
        const UvAxes axes = uv_axes(triangle(quad, t));
        REQUIRE(axes.valid);
        CHECK_THAT(axes.du, VecWithin(0.73 * face.u, tolerance_m));
        CHECK_THAT(axes.dv, VecWithin(-0.92 * face.v, tolerance_m));
    }
}

TEST_CASE("painting_quad: the work's depth moves its face out from the wall", "[geom][painting]") {
    // A canvas 0.03 m deep on the same placement: its back is 0.04 m out, its face
    // 0.05 + 0.04 + 0.03 = 0.12 m on y.
    const FlatWork canvas{.width_m = 0.73, .height_m = 0.92, .depth_m = 0.03};
    const Mesh quad = painting_quad(face_a(), canvas, chart_at);
    REQUIRE(quad.positions.size() == 4);
    CHECK_THAT(quad.positions[0], VecWithin(dvec3(3.365, 0.12, 1.04), corner_tolerance_m));
    CHECK_THAT(quad.positions[1], VecWithin(dvec3(2.635, 0.12, 1.04), corner_tolerance_m));
    CHECK_THAT(quad.positions[2], VecWithin(dvec3(2.635, 0.12, 1.96), corner_tolerance_m));
    CHECK_THAT(quad.positions[3], VecWithin(dvec3(3.365, 0.12, 1.96), corner_tolerance_m));
}

TEST_CASE("painting_quad: on face B, the other side of the wall", "[geom][painting]") {
    // Face B of the same wall: on y = -0.05, facing -y, u along +x from x = 0. A work 0.4 x 0.3 m
    // and 0.01 m deep at u = 1, v = 1.2, stand-off 0.02: its face is on
    // y = -0.05 - 0.02 - 0.01 = -0.08. Seen from the front (from -y), right is +x.
    const FaceFrame face = wall_face_frame({0.0, 0.0}, {6.0, 0.0}, 0.1, WallFace::b);
    const Mesh quad = painting_quad(face, {.width_m = 0.4, .height_m = 0.3, .depth_m = 0.01},
                                    {.u_m = 1.0, .v_m = 1.2, .stand_off_m = 0.02});
    check_mesh_conventions(quad);
    REQUIRE(quad.positions.size() == 4);
    CHECK_THAT(quad.positions[0], VecWithin(dvec3(0.8, -0.08, 1.05), corner_tolerance_m));
    CHECK_THAT(quad.positions[1], VecWithin(dvec3(1.2, -0.08, 1.05), corner_tolerance_m));
    CHECK_THAT(quad.positions[2], VecWithin(dvec3(1.2, -0.08, 1.35), corner_tolerance_m));
    CHECK_THAT(quad.positions[3], VecWithin(dvec3(0.8, -0.08, 1.35), corner_tolerance_m));
    for (const dvec3& n : quad.normals) {
        CHECK_THAT(n, VecWithin(dvec3(0.0, -1.0, 0.0), unit_tolerance));
    }
    CHECK_THAT(area(quad), WithinRel(0.4 * 0.3, 1e-9));
}

TEST_CASE("painting_quad: equals the work's front taken to the world by on_wall_transform",
          "[geom][painting]") {
    // painting.hpp: the rectangle at z = depth in the work's own coordinates, through
    // on_wall_transform(face, at). The two compute the same thing, so only rounding (about
    // 1e-15 m) may separate them: 1e-9 m.
    struct Case {
        const char* name;
        FaceFrame face;
        FlatWork work;
        WallPlacement at;
    };
    const std::array<Case, 3> cases{{
        {"face A, the acceptance criteria", face_a(), chart, chart_at},
        {"face B, a deep panel",
         wall_face_frame({0.0, 0.0}, {6.0, 0.0}, 0.1, WallFace::b),
         {.width_m = 1.2, .height_m = 0.6, .depth_m = 0.05},
         {.u_m = 2.0, .v_m = 1.6, .stand_off_m = 0.01}},
        {"face A of a diagonal wall",
         wall_face_frame({1.0, 2.0}, {4.0, 6.0}, 0.2, WallFace::a),
         {.width_m = 0.5, .height_m = 0.7, .depth_m = 0.02},
         {.u_m = 2.5, .v_m = 1.4, .stand_off_m = 0.03}},
    }};

    for (const Case& c : cases) {
        INFO(c.name);
        const Mesh quad = painting_quad(c.face, c.work, c.at);
        check_mesh_conventions(quad);
        REQUIRE(quad.positions.size() == 4);
        const glm::dmat4 m = on_wall_transform(c.face, c.at);
        const std::array<dvec3, 4> corners = local_corners(c.work);
        for (std::size_t i = 0; i < 4; ++i) {
            INFO("corner " << i);
            CHECK_THAT(quad.positions[i], VecWithin(dvec3(m * dvec4(corners[i], 1.0)), 1e-9));
            CHECK_THAT(quad.normals[i], VecWithin(c.face.normal, unit_tolerance));
        }
        CHECK_THAT(area(quad), WithinRel(c.work.width_m * c.work.height_m, 1e-9));
    }
}

TEST_CASE("painting_quad: the skeleton scene's chart faces the room at (3.0, 0.04, 1.5)",
          "[geom][painting]") {
    // tests/scenes/skeleton/README.md: face A of wall A, from (0, -0.05) to (6, -0.05) and 0.1 m
    // thick, is on y = 0; the chart, a 0.73 x 0.92 m print hung at u = 3.0, v = 1.50 with a
    // stand-off of 0.04, has the centre of its face at (3.0, 0.04, 1.5).
    const FaceFrame wall_a = wall_face_frame({0.0, -0.05}, {6.0, -0.05}, 0.1, WallFace::a);
    const Mesh quad = painting_quad(wall_a, chart, chart_at);
    REQUIRE(quad.positions.size() == 4);
    const dvec3 centre =
        (quad.positions[0] + quad.positions[1] + quad.positions[2] + quad.positions[3]) / 4.0;
    CHECK_THAT(centre, VecWithin(dvec3(3.0, 0.04, 1.5), corner_tolerance_m));
    CHECK_THAT(quad.positions[0], VecWithin(dvec3(3.365, 0.04, 1.04), corner_tolerance_m));
    CHECK_THAT(quad.positions[2], VecWithin(dvec3(2.635, 0.04, 1.96), corner_tolerance_m));
}

TEST_CASE("painting_quad: no depth and no stand-off are allowed", "[geom][painting]") {
    // Only negative values are invalid: a print laid flat on the wall is on the face itself,
    // y = 0.05.
    const Mesh quad = painting_quad(face_a(), chart, {.u_m = 3.0, .v_m = 1.5, .stand_off_m = 0.0});
    REQUIRE(quad.positions.size() == 4);
    for (const dvec3& p : quad.positions) {
        CHECK_THAT(p.y, WithinAbs(0.05, corner_tolerance_m));
    }
}

TEST_CASE("painting_quad: rejects sizes that are not positive, negative offsets and values that "
          "are not finite",
          "[geom][painting]") {
    const FaceFrame face = face_a();
    const auto work = [](double width_m, double height_m, double depth_m) {
        return FlatWork{.width_m = width_m, .height_m = height_m, .depth_m = depth_m};
    };
    const auto at = [](double u_m, double v_m, double stand_off_m) {
        return WallPlacement{.u_m = u_m, .v_m = v_m, .stand_off_m = stand_off_m};
    };

    CHECK_THROWS_AS(painting_quad(face, work(0.0, 0.92, 0.0), chart_at), std::invalid_argument);
    CHECK_THROWS_AS(painting_quad(face, work(-0.73, 0.92, 0.0), chart_at), std::invalid_argument);
    CHECK_THROWS_AS(painting_quad(face, work(0.73, 0.0, 0.0), chart_at), std::invalid_argument);
    CHECK_THROWS_AS(painting_quad(face, work(0.73, -0.92, 0.0), chart_at), std::invalid_argument);
    CHECK_THROWS_AS(painting_quad(face, work(0.73, 0.92, -0.01), chart_at), std::invalid_argument);
    CHECK_THROWS_AS(painting_quad(face, chart, at(3.0, 1.5, -0.04)), std::invalid_argument);

    CHECK_THROWS_AS(painting_quad(face, work(not_a_number, 0.92, 0.0), chart_at),
                    std::invalid_argument);
    CHECK_THROWS_AS(painting_quad(face, work(0.73, infinity, 0.0), chart_at),
                    std::invalid_argument);
    CHECK_THROWS_AS(painting_quad(face, work(0.73, 0.92, not_a_number), chart_at),
                    std::invalid_argument);
    CHECK_THROWS_AS(painting_quad(face, work(0.73, 0.92, infinity), chart_at),
                    std::invalid_argument);
    CHECK_THROWS_AS(painting_quad(face, chart, at(not_a_number, 1.5, 0.04)), std::invalid_argument);
    CHECK_THROWS_AS(painting_quad(face, chart, at(3.0, -infinity, 0.04)), std::invalid_argument);
    CHECK_THROWS_AS(painting_quad(face, chart, at(3.0, 1.5, infinity)), std::invalid_argument);
}
