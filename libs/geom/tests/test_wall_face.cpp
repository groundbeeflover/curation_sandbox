// SPDX-License-Identifier: Apache-2.0

// The faces of a wall and the OnWall placement on them (wall_face.hpp, ADR 0008): u along the face
// from its left end seen facing it, v the height, the normal out of the face, and (u, v, normal)
// right-handed, so that face A's left end is the wall's end and face B's its start.

#include "mesh_checks.hpp"

#include <cs/geom/wall_face.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec4.hpp>

#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>

using namespace cs::geom;
using namespace cs::geom::test;
using Catch::Matchers::WithinAbs;
using glm::dvec2;
using glm::dvec3;
using glm::dvec4;

namespace {

constexpr double infinity = std::numeric_limits<double>::infinity();
constexpr double not_a_number = std::numeric_limits<double>::quiet_NaN();

// The wall of the acceptance criteria: 6 m along x from the origin, 0.1 m thick. Face A is on its
// left seen from above, the +y side, at y = 0.05; face B on the -y side, at y = -0.05.
const dvec2 start{0.0, 0.0};
const dvec2 end{6.0, 0.0};
constexpr double thickness_m = 0.1;

// A local point of a hung thing taken to the world by a transform.
dvec3 apply(const glm::dmat4& m, dvec3 local) {
    return dvec3(m * dvec4(local, 1.0));
}

// A local direction taken to the world by a transform.
dvec3 apply_direction(const glm::dmat4& m, dvec3 local) {
    return dvec3(m * dvec4(local, 0.0));
}

struct WallLine {
    dvec2 start;
    dvec2 end;
    double thickness_m;
};

} // namespace

TEST_CASE("wall_face_frame: (u = 3, v = 1.5) on face A of the wall from (0, 0) to (6, 0) is "
          "(3, 0.05, 1.5), with u along the wall and v up",
          "[geom][wall_face]") {
    const FaceFrame face = wall_face_frame(start, end, thickness_m, WallFace::a);

    // The issue's tolerance, 1e-9 m.
    CHECK_THAT(face.point(3.0, 1.5), VecWithin(dvec3(3.0, 0.05, 1.5), 1e-9));

    // u along the wall (either way along x), v straight up.
    CHECK_THAT(std::abs(glm::dot(face.u, dvec3(1.0, 0.0, 0.0))), WithinAbs(1.0, 1e-9));
    CHECK_THAT(face.v, VecWithin(dvec3(0.0, 0.0, 1.0), 1e-9));
}

TEST_CASE("wall_face_frame: face A runs from the wall's end, its left end seen facing it",
          "[geom][wall_face]") {
    // Standing in front of face A, on the +y side looking towards -y, the wall's end (6, 0) is on
    // the left and its start on the right: u runs along -x from x = 6.
    const FaceFrame face = wall_face_frame(start, end, thickness_m, WallFace::a);

    CHECK_THAT(face.origin, VecWithin(dvec3(6.0, 0.05, 0.0), tolerance_m));
    CHECK_THAT(face.u, VecWithin(dvec3(-1.0, 0.0, 0.0), tolerance_m));
    CHECK_THAT(face.v, VecWithin(dvec3(0.0, 0.0, 1.0), tolerance_m));
    CHECK_THAT(face.normal, VecWithin(dvec3(0.0, 1.0, 0.0), tolerance_m));
    CHECK_THAT(face.length_m, WithinAbs(6.0, tolerance_m));

    // A point off the middle tells the two ends apart: 1 m from the left end is x = 5.
    CHECK_THAT(face.point(1.0, 0.0), VecWithin(dvec3(5.0, 0.05, 0.0), tolerance_m));
    CHECK_THAT(face.point(0.0, 0.0), VecWithin(dvec3(6.0, 0.05, 0.0), tolerance_m));
    CHECK_THAT(face.point(6.0, 2.0), VecWithin(dvec3(0.0, 0.05, 2.0), tolerance_m));

    // w is out of the face, into the room on the +y side.
    CHECK_THAT(face.point(3.0, 1.5, 0.04), VecWithin(dvec3(3.0, 0.09, 1.5), tolerance_m));
}

TEST_CASE("wall_face_frame: face B runs from the wall's start, its left end seen facing it",
          "[geom][wall_face]") {
    // Standing in front of face B, on the -y side looking towards +y, the wall's start (0, 0) is
    // on the left: u runs along +x from x = 0, and the normal is -y.
    const FaceFrame face = wall_face_frame(start, end, thickness_m, WallFace::b);

    CHECK_THAT(face.origin, VecWithin(dvec3(0.0, -0.05, 0.0), tolerance_m));
    CHECK_THAT(face.u, VecWithin(dvec3(1.0, 0.0, 0.0), tolerance_m));
    CHECK_THAT(face.v, VecWithin(dvec3(0.0, 0.0, 1.0), tolerance_m));
    CHECK_THAT(face.normal, VecWithin(dvec3(0.0, -1.0, 0.0), tolerance_m));
    CHECK_THAT(face.length_m, WithinAbs(6.0, tolerance_m));

    CHECK_THAT(face.point(1.0, 0.0), VecWithin(dvec3(1.0, -0.05, 0.0), tolerance_m));
    CHECK_THAT(face.point(3.0, 1.5, 0.04), VecWithin(dvec3(3.0, -0.09, 1.5), tolerance_m));
}

TEST_CASE("wall_face_frame: on a diagonal wall", "[geom][wall_face]") {
    // From (1, 2) to (4, 6): along (3, 4), 5 m long, direction d = (0.6, 0.8). Its left, seen
    // from above, is d turned a quarter anticlockwise: (-0.8, 0.6). 0.2 m thick, so each face is
    // 0.1 m from the centre line.
    const dvec2 s{1.0, 2.0};
    const dvec2 e{4.0, 6.0};

    SECTION("face A: on the left, from the end") {
        // Origin: the end (4, 6) + 0.1 (-0.8, 0.6) = (3.92, 6.06). u = -d.
        const FaceFrame face = wall_face_frame(s, e, 0.2, WallFace::a);
        CHECK_THAT(face.origin, VecWithin(dvec3(3.92, 6.06, 0.0), tolerance_m));
        CHECK_THAT(face.u, VecWithin(dvec3(-0.6, -0.8, 0.0), tolerance_m));
        CHECK_THAT(face.v, VecWithin(dvec3(0.0, 0.0, 1.0), tolerance_m));
        CHECK_THAT(face.normal, VecWithin(dvec3(-0.8, 0.6, 0.0), tolerance_m));
        CHECK_THAT(face.length_m, WithinAbs(5.0, tolerance_m));
        // The middle, 1 m up: the centre line's midpoint (2.5, 4) + 0.1 (-0.8, 0.6).
        CHECK_THAT(face.point(2.5, 1.0), VecWithin(dvec3(2.42, 4.06, 1.0), tolerance_m));
    }
    SECTION("face B: on the right, from the start") {
        // Origin: the start (1, 2) + 0.1 (0.8, -0.6) = (1.08, 1.94). u = d.
        const FaceFrame face = wall_face_frame(s, e, 0.2, WallFace::b);
        CHECK_THAT(face.origin, VecWithin(dvec3(1.08, 1.94, 0.0), tolerance_m));
        CHECK_THAT(face.u, VecWithin(dvec3(0.6, 0.8, 0.0), tolerance_m));
        CHECK_THAT(face.v, VecWithin(dvec3(0.0, 0.0, 1.0), tolerance_m));
        CHECK_THAT(face.normal, VecWithin(dvec3(0.8, -0.6, 0.0), tolerance_m));
        CHECK_THAT(face.length_m, WithinAbs(5.0, tolerance_m));
        CHECK_THAT(face.point(2.5, 1.0), VecWithin(dvec3(2.58, 3.94, 1.0), tolerance_m));
    }
}

TEST_CASE("wall_face_frame: the axes are unit and right-handed, u x v = normal",
          "[geom][wall_face]") {
    const WallLine wall =
        GENERATE(WallLine{{0.0, 0.0}, {6.0, 0.0}, 0.1}, WallLine{{1.0, 2.0}, {4.0, 6.0}, 0.2},
                 WallLine{{6.0, 4.0}, {6.0, 0.0}, 0.1}, WallLine{{-3.0, 5.0}, {-7.0, 2.0}, 0.15},
                 WallLine{{2.0, 1.0}, {2.0 + 1e-3, 1.0}, 0.3});
    const WallFace which = GENERATE(WallFace::a, WallFace::b);
    INFO("wall from " << Catch::StringMaker<dvec2>::convert(wall.start) << " to "
                      << Catch::StringMaker<dvec2>::convert(wall.end) << ", face "
                      << (which == WallFace::a ? "A" : "B"));

    const FaceFrame face = wall_face_frame(wall.start, wall.end, wall.thickness_m, which);

    CHECK_THAT(glm::length(face.u), WithinAbs(1.0, unit_tolerance));
    CHECK_THAT(glm::length(face.v), WithinAbs(1.0, unit_tolerance));
    CHECK_THAT(glm::length(face.normal), WithinAbs(1.0, unit_tolerance));
    CHECK_THAT(face.v, VecWithin(dvec3(0.0, 0.0, 1.0), unit_tolerance));
    CHECK_THAT(face.u.z, WithinAbs(0.0, unit_tolerance));
    CHECK_THAT(face.normal.z, WithinAbs(0.0, unit_tolerance));
    CHECK_THAT(glm::cross(face.u, face.v), VecWithin(face.normal, unit_tolerance));

    // Face A is on the left of the line from start to end, seen from above, and its u runs
    // against that line; face B is on the right and its u runs with it.
    const double length = glm::length(wall.end - wall.start);
    const dvec2 d = (wall.end - wall.start) / length;
    const dvec3 along{d.x, d.y, 0.0};
    const dvec3 left{-d.y, d.x, 0.0};
    const double sign = which == WallFace::a ? 1.0 : -1.0;
    CHECK_THAT(face.normal, VecWithin(sign * left, unit_tolerance));
    CHECK_THAT(face.u, VecWithin(-sign * along, unit_tolerance));
    CHECK_THAT(face.length_m, WithinAbs(length, tolerance_m));

    // The face is thickness / 2 from the centre line, and its origin is at the left end, z = 0.
    const dvec2 left_end = which == WallFace::a ? wall.end : wall.start;
    const dvec3 expected_origin = dvec3(left_end, 0.0) + face.normal * (wall.thickness_m / 2.0);
    CHECK_THAT(face.origin, VecWithin(expected_origin, tolerance_m));
}

TEST_CASE("wall_face_frame: a wall with no thickness has both faces on its centre line",
          "[geom][wall_face]") {
    // Only a negative thickness is invalid.
    const FaceFrame a = wall_face_frame(start, end, 0.0, WallFace::a);
    const FaceFrame b = wall_face_frame(start, end, 0.0, WallFace::b);
    CHECK_THAT(a.origin, VecWithin(dvec3(6.0, 0.0, 0.0), tolerance_m));
    CHECK_THAT(b.origin, VecWithin(dvec3(0.0, 0.0, 0.0), tolerance_m));
    CHECK_THAT(a.normal, VecWithin(-b.normal, tolerance_m));
}

TEST_CASE("wall_face_frame: the skeleton scene's wall A has face A on y = 0, facing the room",
          "[geom][wall_face]") {
    // tests/scenes/skeleton/README.md: wall A runs from (0, -0.05) to (6, -0.05), 0.1 m thick,
    // standing outside the outline with its face A on the edge y = 0 and facing +y, into the room.
    // The chart hangs on it at u = 3, v = 1.5, stand-off 0.04, a print with no depth, so the
    // centre of its face is at (3.0, 0.04, 1.5).
    const FaceFrame face = wall_face_frame({0.0, -0.05}, {6.0, -0.05}, 0.1, WallFace::a);
    CHECK_THAT(face.origin, VecWithin(dvec3(6.0, 0.0, 0.0), tolerance_m));
    CHECK_THAT(face.normal, VecWithin(dvec3(0.0, 1.0, 0.0), tolerance_m));
    CHECK_THAT(face.point(3.0, 1.5, 0.04), VecWithin(dvec3(3.0, 0.04, 1.5), tolerance_m));
}

TEST_CASE("wall_face_frame: rejects equal ends, a negative thickness and values that are not "
          "finite",
          "[geom][wall_face]") {
    CHECK_THROWS_AS(wall_face_frame({1.0, 2.0}, {1.0, 2.0}, 0.1, WallFace::a),
                    std::invalid_argument);
    CHECK_THROWS_AS(wall_face_frame(start, end, -0.1, WallFace::a), std::invalid_argument);
    CHECK_THROWS_AS(wall_face_frame(start, end, -1e-12, WallFace::b), std::invalid_argument);
    CHECK_THROWS_AS(wall_face_frame(start, end, not_a_number, WallFace::a), std::invalid_argument);
    CHECK_THROWS_AS(wall_face_frame(start, end, infinity, WallFace::a), std::invalid_argument);
    CHECK_THROWS_AS(wall_face_frame({not_a_number, 0.0}, end, 0.1, WallFace::a),
                    std::invalid_argument);
    CHECK_THROWS_AS(wall_face_frame(start, {6.0, infinity}, 0.1, WallFace::b),
                    std::invalid_argument);
    CHECK_THROWS_AS(wall_face_frame({-infinity, 0.0}, end, 0.1, WallFace::a),
                    std::invalid_argument);
}

TEST_CASE("on_wall_transform: takes a hung thing's axes to the face's and its origin to the "
          "placement",
          "[geom][wall_face]") {
    // Face A of the wall from (0, 0) to (6, 0): u = -x, v = +z, normal = +y, origin (6, 0.05, 0).
    const FaceFrame face = wall_face_frame(start, end, thickness_m, WallFace::a);
    const glm::dmat4 m = on_wall_transform(face, {.u_m = 3.0, .v_m = 1.5, .stand_off_m = 0.04});

    // x (right seen from the front) -> u, y (up) -> v, z (out of the front) -> the normal.
    CHECK_THAT(apply_direction(m, {1.0, 0.0, 0.0}), VecWithin(dvec3(-1.0, 0.0, 0.0), 1e-12));
    CHECK_THAT(apply_direction(m, {0.0, 1.0, 0.0}), VecWithin(dvec3(0.0, 0.0, 1.0), 1e-12));
    CHECK_THAT(apply_direction(m, {0.0, 0.0, 1.0}), VecWithin(dvec3(0.0, 1.0, 0.0), 1e-12));

    // The origin, the centre of the thing's back: 3 m from x = 6, 1.5 m up, 0.04 m out from
    // y = 0.05.
    CHECK_THAT(apply(m, {0.0, 0.0, 0.0}), VecWithin(dvec3(3.0, 0.09, 1.5), tolerance_m));

    // (0.1, 0.2, 0.03) in the thing is face.point(3.1, 1.7, 0.07) = (6 - 3.1, 0.05 + 0.07, 1.7).
    CHECK_THAT(apply(m, {0.1, 0.2, 0.03}), VecWithin(dvec3(2.9, 0.12, 1.7), tolerance_m));
}

TEST_CASE("on_wall_transform: is a rotation and a translation", "[geom][wall_face]") {
    // Face B of the diagonal wall from (1, 2) to (4, 6), 0.2 m thick.
    const FaceFrame face = wall_face_frame({1.0, 2.0}, {4.0, 6.0}, 0.2, WallFace::b);
    const WallPlacement at{.u_m = 1.0, .v_m = 1.2, .stand_off_m = 0.02};
    const glm::dmat4 m = on_wall_transform(face, at);

    // No projection: the bottom row is (0, 0, 0, 1).
    CHECK(m[0][3] == 0.0);
    CHECK(m[1][3] == 0.0);
    CHECK(m[2][3] == 0.0);
    CHECK(m[3][3] == 1.0);

    // A proper rotation: orthonormal, determinant +1, so nothing is mirrored.
    const glm::dmat3 r(m);
    const glm::dmat3 rtr = glm::transpose(r) * r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            CHECK_THAT(rtr[i][j], WithinAbs(i == j ? 1.0 : 0.0, unit_tolerance));
        }
    }
    CHECK_THAT(glm::determinant(r), WithinAbs(1.0, unit_tolerance));

    // Any local point (x, y, z) lands at face.point(u + x, v + y, stand-off + z).
    for (const dvec3& local :
         {dvec3(0.0, 0.0, 0.0), dvec3(0.3, -0.2, 0.05), dvec3(-1.0, 2.0, 0.5)}) {
        CHECK_THAT(apply(m, local), VecWithin(face.point(at.u_m + local.x, at.v_m + local.y,
                                                         at.stand_off_m + local.z),
                                              tolerance_m));
    }
    // The origin: the start (1, 2) + 0.1 (0.8, -0.6), 1 m along (0.6, 0.8), 1.2 m up and 0.02 m
    // out along (0.8, -0.6): (1.08 + 0.6 + 0.016, 1.94 + 0.8 - 0.012, 1.2).
    CHECK_THAT(apply(m, {0.0, 0.0, 0.0}), VecWithin(dvec3(1.696, 2.728, 1.2), tolerance_m));
}

TEST_CASE("on_wall_transform: rejects a negative stand-off and values that are not finite",
          "[geom][wall_face]") {
    const FaceFrame face = wall_face_frame(start, end, thickness_m, WallFace::a);
    CHECK_NOTHROW(on_wall_transform(face, {.u_m = 3.0, .v_m = 1.5, .stand_off_m = 0.0}));
    CHECK_THROWS_AS(on_wall_transform(face, {.u_m = 3.0, .v_m = 1.5, .stand_off_m = -0.01}),
                    std::invalid_argument);
    CHECK_THROWS_AS(on_wall_transform(face, {.u_m = not_a_number, .v_m = 1.5, .stand_off_m = 0.04}),
                    std::invalid_argument);
    CHECK_THROWS_AS(on_wall_transform(face, {.u_m = 3.0, .v_m = infinity, .stand_off_m = 0.04}),
                    std::invalid_argument);
    CHECK_THROWS_AS(on_wall_transform(face, {.u_m = 3.0, .v_m = 1.5, .stand_off_m = not_a_number}),
                    std::invalid_argument);
    CHECK_THROWS_AS(on_wall_transform(face, {.u_m = 3.0, .v_m = 1.5, .stand_off_m = infinity}),
                    std::invalid_argument);
}

TEST_CASE("wall_face_frame: walls of huge or tiny finite length get unit axes; a length that "
          "overflows is rejected",
          "[geom][wall_face]") {
    // 2e200 m long: the square of the length overflows a double, the length itself does not.
    const FaceFrame long_face = wall_face_frame({-1e200, 0.0}, {1e200, 0.0}, 0.1, WallFace::b);
    CHECK(long_face.length_m == 2e200);
    CHECK_THAT(long_face.u, VecWithin(dvec3(1.0, 0.0, 0.0), unit_tolerance));
    CHECK_THAT(long_face.normal, VecWithin(dvec3(0.0, -1.0, 0.0), unit_tolerance));

    // 1e-160 m long: the square of the length underflows, the length does not.
    const FaceFrame short_face = wall_face_frame({0.0, 0.0}, {0.0, 1e-160}, 0.0, WallFace::a);
    CHECK_THAT(short_face.u, VecWithin(dvec3(0.0, -1.0, 0.0), unit_tolerance));
    CHECK_THAT(short_face.normal, VecWithin(dvec3(-1.0, 0.0, 0.0), unit_tolerance));

    // 3e308 m long, from finite ends: more than the largest double.
    CHECK_THROWS_AS(wall_face_frame({-1.5e308, 0.0}, {1.5e308, 0.0}, 0.1, WallFace::a),
                    std::invalid_argument);
}

TEST_CASE("on_wall_transform: rejects a placement whose position overflows", "[geom][wall_face]") {
    // Face A of a wall ending at x = 1.7e308: its left end is there, and u runs along -x.
    const FaceFrame face = wall_face_frame({0.0, 0.0}, {1.7e308, 0.0}, 0.0, WallFace::a);
    CHECK_NOTHROW(on_wall_transform(face, {.u_m = 1.0, .v_m = 1.5, .stand_off_m = 0.0}));
    // u = -1.7e308 puts the origin at x = 3.4e308, past the largest double.
    CHECK_THROWS_AS(on_wall_transform(face, {.u_m = -1.7e308, .v_m = 1.5, .stand_off_m = 0.0}),
                    std::invalid_argument);
}
