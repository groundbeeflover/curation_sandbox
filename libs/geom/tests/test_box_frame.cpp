// SPDX-License-Identifier: Apache-2.0

// The plain box frame of milestone 1 (box_frame.hpp): a closed ring of four square-section members
// around a work, overlapping it by the rebate, in its own coordinates (x right, y up, z out of the
// front, origin at the centre of its back).

#include "mesh_checks.hpp"

#include <cs/geom/box_frame.hpp>

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

// The issue's tolerance for the sight opening and the outer size: 1 mm.
constexpr double size_tolerance_m = 1e-3;

// The frame of the acceptance criteria: a work 0.73 m wide and 0.92 m high, members 0.06 m wide
// and 0.04 m deep, overlapping the work by 5 mm on each side.
//   sight opening: 0.73 - 2 * 0.005 = 0.72 by 0.92 - 2 * 0.005 = 0.91
//   outer size:    0.72 + 2 * 0.06 = 0.84 by 0.91 + 2 * 0.06 = 1.03
constexpr BoxFrame chart_frame{.work_width_m = 0.73,
                               .work_height_m = 0.92,
                               .member_width_m = 0.06,
                               .depth_m = 0.04,
                               .rebate_m = 0.005};

// What a frame's mesh should measure, from box_frame.hpp's description.
struct Expected {
    double sight_width_m;
    double sight_height_m;
    double outer_width_m;
    double outer_height_m;
    double depth_m;
    double volume_m3; // (outer width * outer height - sight width * sight height) * depth
    double area_m2;   // 2 rings + 2 (outer width + outer height) depth + 2 (sight w + h) depth
};

constexpr Expected chart_expected{
    .sight_width_m = 0.72,
    .sight_height_m = 0.91,
    .outer_width_m = 0.84,
    .outer_height_m = 1.03,
    .depth_m = 0.04,
    // The ring: 0.84 * 1.03 - 0.72 * 0.91 = 0.8652 - 0.6552 = 0.21 m^2; times 0.04 m deep.
    .volume_m3 = 0.0084,
    // Front and back 2 * 0.21 = 0.42; outer sides 2 (0.84 + 1.03) 0.04 = 0.1496; inner sides
    // 2 (0.72 + 0.91) 0.04 = 0.1304. In all 0.70 m^2.
    .area_m2 = 0.70,
};

// The extent of a set of values.
struct Range {
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    void add(double x) {
        lo = x < lo ? x : lo;
        hi = x > hi ? x : hi;
    }
};

// The side directions, in the order the arrays below use: +x, -x, +y, -y.
const std::array<dvec3, 4> side_normals{
    {{1.0, 0.0, 0.0}, {-1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, -1.0, 0.0}}};

// The frame's mesh, sorted by face: front (+z), back (-z), and the sides, outer or inner by
// whether their normal points away from the frame's axis (x = y = 0) or towards it, into the sight
// opening. For each, the area and where its vertices are.
struct FrameFaces {
    double front_m2 = 0.0;
    double back_m2 = 0.0;
    Range front_z;
    Range back_z;
    std::array<double, 4> outer_m2{};
    std::array<double, 4> inner_m2{};
    std::array<Range, 4> outer_at{}; // x of the +x and -x sides, y of the +y and -y sides
    std::array<Range, 4> inner_at{};
    std::size_t unclassified = 0;
};

FrameFaces sort_faces(const Mesh& mesh) {
    // Normals are unit and along the axes: a cosine within 1e-9 of 1 picks one direction.
    constexpr double cos_tolerance = 1e-9;
    FrameFaces faces;
    for (std::size_t t = 0; t < triangle_count(mesh); ++t) {
        const Triangle tri = triangle(mesh, t);
        const dvec3 n = tri.n[0];
        const double a = triangle_area(tri);
        if (n.z >= 1.0 - cos_tolerance) {
            faces.front_m2 += a;
            for (const dvec3& p : tri.p) {
                faces.front_z.add(p.z);
            }
            continue;
        }
        if (n.z <= -1.0 + cos_tolerance) {
            faces.back_m2 += a;
            for (const dvec3& p : tri.p) {
                faces.back_z.add(p.z);
            }
            continue;
        }
        bool sorted = false;
        for (std::size_t k = 0; k < side_normals.size(); ++k) {
            if (glm::dot(n, side_normals[k]) < 1.0 - cos_tolerance) {
                continue;
            }
            const dvec3 c = centroid(tri);
            const bool inner = glm::dot(n, dvec3(c.x, c.y, 0.0)) < 0.0;
            (inner ? faces.inner_m2 : faces.outer_m2)[k] += a;
            for (const dvec3& p : tri.p) {
                (inner ? faces.inner_at : faces.outer_at)[k].add(k < 2 ? p.x : p.y);
            }
            sorted = true;
            break;
        }
        if (!sorted) {
            ++faces.unclassified;
        }
    }
    return faces;
}

void check_range(const Range& range, double expected, double tolerance) {
    CHECK_THAT(range.lo, WithinAbs(expected, tolerance));
    CHECK_THAT(range.hi, WithinAbs(expected, tolerance));
}

// Checks a frame's mesh against what box_frame.hpp says it is. The header defines every size
// exactly from the inputs, so only rounding (about 1e-16 m) separates the mesh from them: 1e-9 m
// for positions and areas, 1e-9 m^3 for the volume (the chart frame's is 0.0084 m^3).
void check_frame_mesh(const Mesh& mesh, const Expected& e) {
    check_mesh_conventions(mesh);
    check_uvs_in_metres(mesh);
    CHECK(is_closed(mesh));
    CHECK_THAT(volume(mesh), WithinAbs(e.volume_m3, 1e-9));
    CHECK_THAT(area(mesh), WithinAbs(e.area_m2, 1e-9));

    const double sx = e.sight_width_m / 2.0;
    const double sy = e.sight_height_m / 2.0;
    const double ox = e.outer_width_m / 2.0;
    const double oy = e.outer_height_m / 2.0;
    const double d = e.depth_m;

    const Bounds b = bounds(mesh);
    CHECK_THAT(b.lo, VecWithin(dvec3(-ox, -oy, 0.0), tolerance_m));
    CHECK_THAT(b.hi, VecWithin(dvec3(ox, oy, d), tolerance_m));

    const FrameFaces faces = sort_faces(mesh);
    CHECK(faces.unclassified == 0);

    // Front and back: each the ring between the outer rectangle and the sight opening.
    const double ring_m2 = e.outer_width_m * e.outer_height_m - e.sight_width_m * e.sight_height_m;
    CHECK_THAT(faces.front_m2, WithinAbs(ring_m2, tolerance_m));
    CHECK_THAT(faces.back_m2, WithinAbs(ring_m2, tolerance_m));
    check_range(faces.front_z, d, tolerance_m);
    check_range(faces.back_z, 0.0, tolerance_m);

    // Outer sides on x = +-ox and y = +-oy; inner sides on x = -+sx and y = -+sy: the inner side
    // facing +x is the left member's, at x = -sx.
    const std::array<double, 4> outer_at{ox, -ox, oy, -oy};
    const std::array<double, 4> inner_at{-sx, sx, -sy, sy};
    const std::array<double, 4> outer_m2{e.outer_height_m * d, e.outer_height_m * d,
                                         e.outer_width_m * d, e.outer_width_m * d};
    const std::array<double, 4> inner_m2{e.sight_height_m * d, e.sight_height_m * d,
                                         e.sight_width_m * d, e.sight_width_m * d};
    const std::array<const char*, 4> names{"+x", "-x", "+y", "-y"};
    for (std::size_t k = 0; k < 4; ++k) {
        INFO("the sides facing " << names[k]);
        CHECK_THAT(faces.outer_m2[k], WithinAbs(outer_m2[k], tolerance_m));
        CHECK_THAT(faces.inner_m2[k], WithinAbs(inner_m2[k], tolerance_m));
        check_range(faces.outer_at[k], outer_at[k], tolerance_m);
        check_range(faces.inner_at[k], inner_at[k], tolerance_m);
    }

    // No vertex of the front or back lies inside the sight opening.
    std::size_t in_opening = 0;
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
        const dvec3& p = mesh.positions[i];
        if (std::abs(mesh.normals[i].z) > 0.5 && std::abs(p.x) < sx - tolerance_m &&
            std::abs(p.y) < sy - tolerance_m) {
            ++in_opening;
        }
    }
    CHECK(in_opening == 0);
}

} // namespace

TEST_CASE("box_frame: sight opening 0.72 x 0.91 and outer size 0.84 x 1.03, within 1 mm",
          "[geom][box_frame]") {
    CHECK_THAT(sight_opening(chart_frame), VecWithin(dvec2(0.72, 0.91), size_tolerance_m));
    CHECK_THAT(outer_size(chart_frame), VecWithin(dvec2(0.84, 1.03), size_tolerance_m));
}

TEST_CASE("box_frame: the mesh spans the outer size, from its back at z = 0 to its front at "
          "z = 0.04",
          "[geom][box_frame]") {
    // Centred on the origin: x from -0.84 / 2 to 0.84 / 2, y from -1.03 / 2 to 1.03 / 2.
    const Mesh frame = box_frame(chart_frame);
    const Bounds b = bounds(frame);
    CHECK_THAT(b.lo, VecWithin(dvec3(-0.42, -0.515, 0.0), size_tolerance_m));
    CHECK_THAT(b.hi, VecWithin(dvec3(0.42, 0.515, 0.04), size_tolerance_m));
    CHECK_THAT(b.hi - b.lo, VecWithin(dvec3(0.84, 1.03, 0.04), size_tolerance_m));
}

TEST_CASE("box_frame: the inner sides face into a sight opening of 0.72 x 0.91, within 1 mm",
          "[geom][box_frame]") {
    // The inner side facing -x is the right member's, at x = 0.72 / 2 = 0.36; the one facing +x
    // the left member's, at -0.36. Facing -y, the top member's at y = 0.91 / 2 = 0.455; facing +y,
    // the bottom member's at -0.455.
    const Mesh frame = box_frame(chart_frame);
    check_mesh_conventions(frame);
    const FrameFaces faces = sort_faces(frame);

    // +x, -x, +y, -y
    const std::array<double, 4> inner_at{-0.36, 0.36, -0.455, 0.455};
    for (std::size_t k = 0; k < 4; ++k) {
        INFO("the inner side facing direction " << k << " of +x, -x, +y, -y");
        REQUIRE(faces.inner_m2[k] > 0.0);
        check_range(faces.inner_at[k], inner_at[k], size_tolerance_m);
    }
    const double sight_width = faces.inner_at[1].lo - faces.inner_at[0].hi;
    const double sight_height = faces.inner_at[3].lo - faces.inner_at[2].hi;
    CHECK_THAT(sight_width, WithinAbs(0.72, size_tolerance_m));
    CHECK_THAT(sight_height, WithinAbs(0.91, size_tolerance_m));
}

TEST_CASE("box_frame: the mesh is closed, every edge shared by two triangles",
          "[geom][box_frame]") {
    CHECK(is_closed(box_frame(chart_frame)));
}

TEST_CASE("box_frame: volume (0.84 x 1.03 - 0.72 x 0.91) x 0.04 = 0.0084 m^3, positive because "
          "it faces outwards",
          "[geom][box_frame]") {
    // Only rounding separates the mesh from the exact ring (see check_frame_mesh): 1e-9 m^3.
    CHECK_THAT(volume(box_frame(chart_frame)), WithinAbs(0.0084, 1e-9));
}

TEST_CASE("box_frame: keeps the mesh conventions, with UVs in metres", "[geom][box_frame]") {
    const Mesh frame = box_frame(chart_frame);
    check_mesh_conventions(frame);
    check_uvs_in_metres(frame);
}

TEST_CASE("box_frame: front, back and the outer and inner sides, with their areas",
          "[geom][box_frame]") {
    check_frame_mesh(box_frame(chart_frame), chart_expected);
}

TEST_CASE("box_frame: other frames", "[geom][box_frame]") {
    SECTION("no rebate: the sight opening is the whole work") {
        // Work 0.5 x 0.3, members 0.1 wide and 0.05 deep: outer 0.7 x 0.5. Ring
        // 0.35 - 0.15 = 0.2 m^2, volume 0.2 * 0.05 = 0.01 m^3; area 2 * 0.2 + 2 (0.7 + 0.5) 0.05
        // + 2 (0.5 + 0.3) 0.05 = 0.4 + 0.12 + 0.08 = 0.6 m^2.
        const BoxFrame f{.work_width_m = 0.5,
                         .work_height_m = 0.3,
                         .member_width_m = 0.1,
                         .depth_m = 0.05,
                         .rebate_m = 0.0};
        CHECK_THAT(sight_opening(f), VecWithin(dvec2(0.5, 0.3), tolerance_m));
        CHECK_THAT(outer_size(f), VecWithin(dvec2(0.7, 0.5), tolerance_m));
        check_frame_mesh(box_frame(f), {.sight_width_m = 0.5,
                                        .sight_height_m = 0.3,
                                        .outer_width_m = 0.7,
                                        .outer_height_m = 0.5,
                                        .depth_m = 0.05,
                                        .volume_m3 = 0.01,
                                        .area_m2 = 0.6});
    }
    SECTION("a landscape work in a wide, deep frame (ADR 0003's gilt cassetta, 10 x 5 cm)") {
        // Work 1.2 x 0.8, rebate 0.01: sight 1.18 x 0.78, outer 1.38 x 0.98. Ring
        // 1.3524 - 0.9204 = 0.432 m^2, volume 0.432 * 0.05 = 0.0216 m^3; area 2 * 0.432
        // + 2 (1.38 + 0.98) 0.05 + 2 (1.18 + 0.78) 0.05 = 0.864 + 0.236 + 0.196 = 1.296 m^2.
        const BoxFrame f{.work_width_m = 1.2,
                         .work_height_m = 0.8,
                         .member_width_m = 0.1,
                         .depth_m = 0.05,
                         .rebate_m = 0.01};
        CHECK_THAT(sight_opening(f), VecWithin(dvec2(1.18, 0.78), tolerance_m));
        CHECK_THAT(outer_size(f), VecWithin(dvec2(1.38, 0.98), tolerance_m));
        check_frame_mesh(box_frame(f), {.sight_width_m = 1.18,
                                        .sight_height_m = 0.78,
                                        .outer_width_m = 1.38,
                                        .outer_height_m = 0.98,
                                        .depth_m = 0.05,
                                        .volume_m3 = 0.0216,
                                        .area_m2 = 1.296});
    }
}

TEST_CASE("box_frame: rejects sizes that are not positive, a negative rebate, an empty sight "
          "opening and values that are not finite",
          "[geom][box_frame]") {
    const auto frame = [](double width, double height, double member, double depth, double rebate) {
        return BoxFrame{.work_width_m = width,
                        .work_height_m = height,
                        .member_width_m = member,
                        .depth_m = depth,
                        .rebate_m = rebate};
    };

    // Sizes that are not positive.
    CHECK_THROWS_AS(box_frame(frame(0.0, 0.92, 0.06, 0.04, 0.005)), std::invalid_argument);
    CHECK_THROWS_AS(box_frame(frame(-0.73, 0.92, 0.06, 0.04, 0.005)), std::invalid_argument);
    CHECK_THROWS_AS(box_frame(frame(0.73, 0.0, 0.06, 0.04, 0.005)), std::invalid_argument);
    CHECK_THROWS_AS(box_frame(frame(0.73, -0.92, 0.06, 0.04, 0.005)), std::invalid_argument);
    CHECK_THROWS_AS(box_frame(frame(0.73, 0.92, 0.0, 0.04, 0.005)), std::invalid_argument);
    CHECK_THROWS_AS(box_frame(frame(0.73, 0.92, -0.06, 0.04, 0.005)), std::invalid_argument);
    CHECK_THROWS_AS(box_frame(frame(0.73, 0.92, 0.06, 0.0, 0.005)), std::invalid_argument);
    CHECK_THROWS_AS(box_frame(frame(0.73, 0.92, 0.06, -0.04, 0.005)), std::invalid_argument);

    // A negative rebate.
    CHECK_THROWS_AS(box_frame(frame(0.73, 0.92, 0.06, 0.04, -0.005)), std::invalid_argument);

    // An empty sight opening: twice the rebate at least the work's width or height. The
    // boundary cases are exact in binary: 0.5 - 2 * 0.25 = 0.
    CHECK_THROWS_AS(box_frame(frame(0.73, 0.92, 0.06, 0.04, 0.4)), std::invalid_argument);
    CHECK_THROWS_AS(box_frame(frame(0.5, 2.0, 0.06, 0.04, 0.25)), std::invalid_argument);
    CHECK_THROWS_AS(box_frame(frame(2.0, 0.5, 0.06, 0.04, 0.25)), std::invalid_argument);
    CHECK_THROWS_AS(box_frame(frame(2.0, 0.5, 0.06, 0.04, 0.3)), std::invalid_argument);

    // Values that are not finite.
    CHECK_THROWS_AS(box_frame(frame(not_a_number, 0.92, 0.06, 0.04, 0.005)), std::invalid_argument);
    CHECK_THROWS_AS(box_frame(frame(infinity, 0.92, 0.06, 0.04, 0.005)), std::invalid_argument);
    CHECK_THROWS_AS(box_frame(frame(0.73, not_a_number, 0.06, 0.04, 0.005)), std::invalid_argument);
    CHECK_THROWS_AS(box_frame(frame(0.73, infinity, 0.06, 0.04, 0.005)), std::invalid_argument);
    CHECK_THROWS_AS(box_frame(frame(0.73, 0.92, not_a_number, 0.04, 0.005)), std::invalid_argument);
    CHECK_THROWS_AS(box_frame(frame(0.73, 0.92, infinity, 0.04, 0.005)), std::invalid_argument);
    CHECK_THROWS_AS(box_frame(frame(0.73, 0.92, 0.06, not_a_number, 0.005)), std::invalid_argument);
    CHECK_THROWS_AS(box_frame(frame(0.73, 0.92, 0.06, infinity, 0.005)), std::invalid_argument);
    CHECK_THROWS_AS(box_frame(frame(0.73, 0.92, 0.06, 0.04, not_a_number)), std::invalid_argument);
    CHECK_THROWS_AS(box_frame(frame(0.73, 0.92, 0.06, 0.04, infinity)), std::invalid_argument);
}
