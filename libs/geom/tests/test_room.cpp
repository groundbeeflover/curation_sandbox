// SPDX-License-Identifier: Apache-2.0

// The inside of a box room (room.hpp): the skeleton scene's gallery, 6 m along x, 4 m along y and
// 3.5 m high, with its floor, ceiling and four walls facing in, and UVs in metres.

#include "mesh_checks.hpp"

#include <cs/geom/room.hpp>
#include <cs/geom/wall_face.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>

using namespace cs::geom;
using namespace cs::geom::test;
using Catch::Matchers::WithinAbs;
using glm::dvec2;
using glm::dvec3;

namespace {

constexpr double infinity = std::numeric_limits<double>::infinity();
constexpr double not_a_number = std::numeric_limits<double>::quiet_NaN();

// The skeleton's gallery (tests/scenes/skeleton/README.md).
constexpr double width_m = 6.0;  // along x
constexpr double depth_m = 4.0;  // along y
constexpr double height_m = 3.5; // along z

// One of the room's six faces, as room.hpp describes it.
struct RoomFace {
    const char* name;
    dvec3 normal;   // into the room
    double plane_m; // dot(normal, p) for every point p of the face
    double area_m2;
    dvec2 (*uv)(const dvec3& p); // the UV of a point of the face, in metres
};

// The walls' UVs are those of face A of a wall standing outside each side of the outline, which
// runs anticlockwise (0, 0), (6, 0), (6, 4), (0, 4). Face A's u runs from the wall's end, its left
// end seen facing it (wall_face.hpp), and v is the height:
// - side (0, 0) -> (6, 0): seen from inside, facing -y, the left end is (6, 0): u = 6 - x.
// - side (6, 0) -> (6, 4): facing +x, the left end is (6, 4): u = 4 - y.
// - side (6, 4) -> (0, 4): facing +y, the left end is (0, 4): u = x.
// - side (0, 4) -> (0, 0): facing -x, the left end is (0, 0): u = y.
// Areas: floor and ceiling 6 * 4 = 24 m^2; walls along x 6 * 3.5 = 21 m^2, along y 4 * 3.5 =
// 14 m^2. In all 2 * 24 + 2 * 21 + 2 * 14 = 118 m^2.
const std::array<RoomFace, 6> faces{{
    {"floor", {0.0, 0.0, 1.0}, 0.0, 24.0, [](const dvec3& p) { return dvec2(p.x, p.y); }},
    {"ceiling",
     {0.0, 0.0, -1.0},
     -height_m,
     24.0,
     [](const dvec3& p) { return dvec2(p.x, depth_m - p.y); }},
    {"wall on y = 0",
     {0.0, 1.0, 0.0},
     0.0,
     21.0,
     [](const dvec3& p) { return dvec2(width_m - p.x, p.z); }},
    {"wall on x = 6",
     {-1.0, 0.0, 0.0},
     -width_m,
     14.0,
     [](const dvec3& p) { return dvec2(depth_m - p.y, p.z); }},
    {"wall on y = 4",
     {0.0, -1.0, 0.0},
     -depth_m,
     21.0,
     [](const dvec3& p) { return dvec2(p.x, p.z); }},
    {"wall on x = 0", {1.0, 0.0, 0.0}, 0.0, 14.0, [](const dvec3& p) { return dvec2(p.y, p.z); }},
}};

// Which of the six faces a normal belongs to, or -1. The normals are unit vectors along the axes,
// so a cosine within 1e-9 of 1 picks one face and only one.
int face_of(const dvec3& normal) {
    for (std::size_t k = 0; k < faces.size(); ++k) {
        if (glm::dot(normal, faces[k].normal) >= 1.0 - 1e-9) {
            return static_cast<int>(k);
        }
    }
    return -1;
}

Mesh gallery() {
    return room_mesh(width_m, depth_m, height_m);
}

} // namespace

TEST_CASE("room_mesh(6, 4, 3.5): keeps the mesh conventions, with UVs in metres", "[geom][room]") {
    const Mesh room = gallery();
    check_mesh_conventions(room);
    check_uvs_in_metres(room);
}

TEST_CASE("room_mesh(6, 4, 3.5): a floor, a ceiling and four walls, with inward normals",
          "[geom][room]") {
    const Mesh room = gallery();
    check_mesh_conventions(room);

    // Every triangle belongs to one of the six faces, by its normal, and lies in that face's
    // plane; the faces have their full areas.
    std::array<double, 6> area_m2{};
    std::size_t unclassified = 0;
    std::size_t off_plane = 0;
    for (std::size_t t = 0; t < triangle_count(room); ++t) {
        const Triangle tri = triangle(room, t);
        const int k = face_of(tri.n[0]);
        if (k < 0) {
            ++unclassified;
            continue;
        }
        const RoomFace& face = faces[static_cast<std::size_t>(k)];
        area_m2[static_cast<std::size_t>(k)] += triangle_area(tri);
        for (const dvec3& p : tri.p) {
            if (!(std::abs(glm::dot(face.normal, p) - face.plane_m) <= tolerance_m)) {
                ++off_plane;
            }
        }
    }
    CHECK(unclassified == 0);
    CHECK(off_plane == 0);
    for (std::size_t k = 0; k < faces.size(); ++k) {
        INFO(faces[k].name);
        CHECK_THAT(area_m2[k], WithinAbs(faces[k].area_m2, tolerance_m));
    }

    // Every normal points towards the room's centre.
    const dvec3 centre{width_m / 2.0, depth_m / 2.0, height_m / 2.0};
    std::size_t outward = 0;
    for (std::size_t i = 0; i < room.positions.size(); ++i) {
        if (!(glm::dot(room.normals[i], centre - room.positions[i]) > 0.0)) {
            ++outward;
        }
    }
    CHECK(outward == 0);

    // The room runs from (0, 0, 0) to (6, 4, 3.5): width along x, depth along y.
    const Bounds b = bounds(room);
    CHECK_THAT(b.lo, VecWithin(dvec3(0.0, 0.0, 0.0), tolerance_m));
    CHECK_THAT(b.hi, VecWithin(dvec3(width_m, depth_m, height_m), tolerance_m));
}

TEST_CASE("room_mesh(6, 4, 3.5): total area 118 m^2", "[geom][room]") {
    // 2 * 6 * 4 + 2 * 6 * 3.5 + 2 * 4 * 3.5 = 48 + 42 + 28 = 118 m^2, within the issue's 1e-9.
    CHECK_THAT(area(gallery()), WithinAbs(118.0, 1e-9));
}

TEST_CASE("room_mesh(6, 4, 3.5): closed, with volume -84 m^3 because it faces inwards",
          "[geom][room]") {
    // room.hpp: volume() is -width * depth * height = -6 * 4 * 3.5 = -84 m^3. The corners are
    // exact in binary, so 1e-9 m^3 allows only rounding.
    const Mesh room = gallery();
    CHECK(is_closed(room));
    CHECK_THAT(volume(room), WithinAbs(-84.0, 1e-9));
}

TEST_CASE("room_mesh(6, 4, 3.5): UVs are metres on each face", "[geom][room]") {
    // Floor (x, y); ceiling (x, 4 - y); walls as in `faces` above.
    const Mesh room = gallery();
    check_mesh_conventions(room);

    std::size_t wrong = 0;
    std::size_t first_wrong = 0;
    for (std::size_t t = 0; t < triangle_count(room); ++t) {
        const Triangle tri = triangle(room, t);
        const int k = face_of(tri.n[0]);
        REQUIRE(k >= 0);
        const RoomFace& face = faces[static_cast<std::size_t>(k)];
        for (std::size_t c = 0; c < 3; ++c) {
            const dvec2 expected = face.uv(tri.p[c]);
            if (!(glm::length(tri.uv[c] - expected) <= tolerance_m)) {
                if (wrong == 0) {
                    first_wrong = t;
                }
                ++wrong;
            }
        }
    }
    INFO("first triangle with a wrong UV: " << first_wrong);
    CHECK(wrong == 0);
}

TEST_CASE("room_mesh(6, 4, 3.5): the walls are the faces A of walls standing outside the outline",
          "[geom][room]") {
    // As in the skeleton scene: each wall is 0.1 m thick, its centre line 0.05 m outside a side
    // of the outline and running with it, anticlockwise, so that its face A is on the side and
    // faces into the room. The room's wall surfaces are those faces: same plane, same normal, and
    // UVs that are the faces' (u, v).
    struct Side {
        dvec2 from;
        dvec2 to;
        dvec2 outward;
        std::size_t face; // into `faces`
    };
    const std::array<Side, 4> sides{{
        {{0.0, 0.0}, {width_m, 0.0}, {0.0, -1.0}, 2},
        {{width_m, 0.0}, {width_m, depth_m}, {1.0, 0.0}, 3},
        {{width_m, depth_m}, {0.0, depth_m}, {0.0, 1.0}, 4},
        {{0.0, depth_m}, {0.0, 0.0}, {-1.0, 0.0}, 5},
    }};

    const Mesh room = gallery();
    check_mesh_conventions(room);

    for (const Side& side : sides) {
        INFO(faces[side.face].name);
        const FaceFrame frame = wall_face_frame(side.from + side.outward * 0.05,
                                                side.to + side.outward * 0.05, 0.1, WallFace::a);
        CHECK_THAT(frame.normal, VecWithin(faces[side.face].normal, unit_tolerance));

        std::size_t vertices = 0;
        std::size_t off_face = 0;
        std::size_t wrong_uv = 0;
        for (std::size_t t = 0; t < triangle_count(room); ++t) {
            const Triangle tri = triangle(room, t);
            if (face_of(tri.n[0]) != static_cast<int>(side.face)) {
                continue;
            }
            for (std::size_t c = 0; c < 3; ++c) {
                ++vertices;
                const dvec3 d = tri.p[c] - frame.origin;
                const double u = glm::dot(d, frame.u);
                const double v = glm::dot(d, frame.v);
                if (!(std::abs(glm::dot(d, frame.normal)) <= tolerance_m) ||
                    !(u >= -tolerance_m && u <= frame.length_m + tolerance_m)) {
                    ++off_face;
                }
                if (!(glm::length(tri.uv[c] - dvec2(u, v)) <= tolerance_m)) {
                    ++wrong_uv;
                }
            }
        }
        CHECK(vertices > 0);
        CHECK(off_face == 0);
        CHECK(wrong_uv == 0);
    }
}

TEST_CASE("room_mesh: other sizes", "[geom][room]") {
    SECTION("2.5 x 7 x 3 m") {
        // Area 2 * 2.5 * 7 + 2 * 2.5 * 3 + 2 * 7 * 3 = 35 + 15 + 42 = 92 m^2; volume
        // -2.5 * 7 * 3 = -52.5 m^3.
        const Mesh room = room_mesh(2.5, 7.0, 3.0);
        check_mesh_conventions(room);
        check_uvs_in_metres(room);
        CHECK(is_closed(room));
        CHECK_THAT(area(room), WithinAbs(92.0, 1e-9));
        CHECK_THAT(volume(room), WithinAbs(-52.5, 1e-9));
        const Bounds b = bounds(room);
        CHECK_THAT(b.lo, VecWithin(dvec3(0.0, 0.0, 0.0), tolerance_m));
        CHECK_THAT(b.hi, VecWithin(dvec3(2.5, 7.0, 3.0), tolerance_m));
    }
    SECTION("5.3 x 4.7 x 3.1 m, not exact in binary") {
        // Area 2 (5.3 * 4.7 + 5.3 * 3.1 + 4.7 * 3.1) = 2 (24.91 + 16.43 + 14.57) = 111.82 m^2;
        // volume -5.3 * 4.7 * 3.1 = -77.221 m^3. Rounding of the inputs is about 1e-15 relative.
        const Mesh room = room_mesh(5.3, 4.7, 3.1);
        check_mesh_conventions(room);
        check_uvs_in_metres(room);
        CHECK(is_closed(room));
        CHECK_THAT(area(room), WithinAbs(111.82, 1e-9));
        CHECK_THAT(volume(room), WithinAbs(-77.221, 1e-9));
    }
}

TEST_CASE("room_mesh: rejects sizes that are not positive and finite", "[geom][room]") {
    CHECK_THROWS_AS(room_mesh(0.0, 4.0, 3.5), std::invalid_argument);
    CHECK_THROWS_AS(room_mesh(6.0, 0.0, 3.5), std::invalid_argument);
    CHECK_THROWS_AS(room_mesh(6.0, 4.0, 0.0), std::invalid_argument);
    CHECK_THROWS_AS(room_mesh(-6.0, 4.0, 3.5), std::invalid_argument);
    CHECK_THROWS_AS(room_mesh(6.0, -4.0, 3.5), std::invalid_argument);
    CHECK_THROWS_AS(room_mesh(6.0, 4.0, -3.5), std::invalid_argument);
    CHECK_THROWS_AS(room_mesh(not_a_number, 4.0, 3.5), std::invalid_argument);
    CHECK_THROWS_AS(room_mesh(6.0, not_a_number, 3.5), std::invalid_argument);
    CHECK_THROWS_AS(room_mesh(6.0, 4.0, not_a_number), std::invalid_argument);
    CHECK_THROWS_AS(room_mesh(infinity, 4.0, 3.5), std::invalid_argument);
    CHECK_THROWS_AS(room_mesh(6.0, infinity, 3.5), std::invalid_argument);
    CHECK_THROWS_AS(room_mesh(6.0, 4.0, infinity), std::invalid_argument);
}
