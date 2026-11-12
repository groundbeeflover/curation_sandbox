// SPDX-License-Identifier: Apache-2.0

// The measurements of mesh.hpp (area, volume, is_closed) and append, on small meshes built here by
// hand, whose answers are known: a unit cube has an area of 6 m^2 and a volume of 1 m^3.

#include "mesh_checks.hpp"

#include <cs/geom/mesh.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

using namespace cs::geom;
using namespace cs::geom::test;
using Catch::Matchers::WithinAbs;
using glm::dvec3;

namespace {

// The meshes here have corners at small integers or halves, exact in binary, and their areas and
// volumes are sums of a few dozen products of them: exact to about 1e-15. 1e-12 allows any order
// of summation.
constexpr double tight = 1e-12;

constexpr double infinity = std::numeric_limits<double>::infinity();
constexpr double not_a_number = std::numeric_limits<double>::quiet_NaN();

// Appends the flat quad p0 p1 p2 p3, counter-clockwise seen from its front, as two triangles with
// four vertices of its own, as mesh.hpp's faces have.
void add_quad(Mesh& mesh, dvec3 p0, dvec3 p1, dvec3 p2, dvec3 p3) {
    const auto base = static_cast<std::uint32_t>(mesh.positions.size());
    const dvec3 normal = glm::normalize(glm::cross(p1 - p0, p2 - p0));
    for (const dvec3& p : {p0, p1, p2, p3}) {
        mesh.positions.push_back(p);
        mesh.normals.push_back(normal);
    }
    mesh.uvs.insert(mesh.uvs.end(), {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}});
    mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

// The axis-aligned box from lo to hi, facing outwards: six faces of four vertices each, 24
// vertices and 12 triangles. Each face's corners run counter-clockwise seen from outside; the
// comment gives the outward normal, which cross(p1 - p0, p2 - p0) confirms.
Mesh box(dvec3 lo, dvec3 hi) {
    const double x0 = lo.x, y0 = lo.y, z0 = lo.z;
    const double x1 = hi.x, y1 = hi.y, z1 = hi.z;
    Mesh mesh;
    add_quad(mesh, {x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}); // bottom, -z
    add_quad(mesh, {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}); // top, +z
    add_quad(mesh, {x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}); // -y
    add_quad(mesh, {x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}); // +y
    add_quad(mesh, {x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}); // -x
    add_quad(mesh, {x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}); // +x
    return mesh;
}

Mesh unit_cube() {
    return box({0.0, 0.0, 0.0}, {1.0, 1.0, 1.0});
}

// Reverses the winding of triangle t.
void flip_triangle(Mesh& mesh, std::size_t t) {
    std::swap(mesh.indices[3 * t + 1], mesh.indices[3 * t + 2]);
}

void remove_triangle(Mesh& mesh, std::size_t t) {
    const auto first = mesh.indices.begin() + static_cast<std::ptrdiff_t>(3 * t);
    mesh.indices.erase(first, first + 3);
}

// Moves every vertex at `corner`, on all the faces that share it, to `to`.
void move_corner(Mesh& mesh, dvec3 corner, dvec3 to) {
    for (dvec3& p : mesh.positions) {
        if (p == corner) {
            p = to;
        }
    }
}

// Adds a vertex and returns its index.
std::uint32_t add_vertex(Mesh& mesh, dvec3 p) {
    mesh.positions.push_back(p);
    mesh.normals.push_back({0.0, 0.0, 1.0});
    mesh.uvs.push_back({0.0, 0.0});
    return static_cast<std::uint32_t>(mesh.positions.size() - 1);
}

} // namespace

TEST_CASE("mesh: a unit cube is closed, with area 6 m^2 and volume 1 m^3", "[geom][mesh]") {
    const Mesh cube = unit_cube();

    // The hand-built cube keeps the conventions, and its normals face outwards.
    check_mesh_conventions(cube);
    REQUIRE(cube.positions.size() == 24);
    REQUIRE(cube.indices.size() == 36);
    const dvec3 centre{0.5, 0.5, 0.5};
    for (std::size_t i = 0; i < cube.positions.size(); ++i) {
        REQUIRE(glm::dot(cube.normals[i], cube.positions[i] - centre) > 0.0);
    }

    CHECK(is_closed(cube));
    CHECK_THAT(area(cube), WithinAbs(6.0, tight));
    CHECK_THAT(volume(cube), WithinAbs(1.0, tight));
}

TEST_CASE("mesh: a box's area and volume follow from its sides", "[geom][mesh]") {
    // A box 2 x 3 x 4 m: area 2 (2*3 + 2*4 + 3*4) = 52 m^2, volume 2*3*4 = 24 m^3, wherever it is.
    SECTION("at the origin") {
        const Mesh b = box({0.0, 0.0, 0.0}, {2.0, 3.0, 4.0});
        CHECK(is_closed(b));
        CHECK_THAT(area(b), WithinAbs(52.0, tight));
        CHECK_THAT(volume(b), WithinAbs(24.0, tight));
    }
    SECTION("away from the origin") {
        const Mesh b = box({-1.0, 0.5, 2.0}, {1.0, 3.5, 6.0});
        CHECK(is_closed(b));
        CHECK_THAT(area(b), WithinAbs(52.0, tight));
        CHECK_THAT(volume(b), WithinAbs(24.0, tight));
    }
}

TEST_CASE("mesh: area adds up the triangles' areas", "[geom][mesh]") {
    Mesh mesh;
    add_vertex(mesh, {0.0, 0.0, 0.0});
    add_vertex(mesh, {3.0, 0.0, 0.0});
    add_vertex(mesh, {0.0, 4.0, 0.0});
    add_vertex(mesh, {0.0, 1.0, 1.0});
    add_vertex(mesh, {1.0, 0.0, 0.0});

    // A right triangle with legs of 3 and 4 m: 3 * 4 / 2 = 6 m^2.
    mesh.indices = {0, 1, 2};
    CHECK_THAT(area(mesh), WithinAbs(6.0, tight));

    // Plus a tilted one, (0,0,0) (1,0,0) (0,1,1): |(1,0,0) x (0,1,1)| / 2 = |(0,-1,1)| / 2 =
    // sqrt(2) / 2 m^2. Its winding does not change its area.
    mesh.indices = {0, 1, 2, 0, 4, 3};
    CHECK_THAT(area(mesh), WithinAbs(6.0 + std::sqrt(2.0) / 2.0, tight));
    mesh.indices = {0, 1, 2, 0, 3, 4};
    CHECK_THAT(area(mesh), WithinAbs(6.0 + std::sqrt(2.0) / 2.0, tight));

    // Lone triangles have edges with only one triangle on them.
    CHECK_FALSE(is_closed(mesh));
}

TEST_CASE("mesh: a cube with a triangle missing is not closed", "[geom][mesh]") {
    Mesh cube = unit_cube();
    remove_triangle(cube, 7);
    CHECK_FALSE(is_closed(cube));
}

TEST_CASE("mesh: a cube with one triangle wound the wrong way is not closed", "[geom][mesh]") {
    // Every edge still has two triangles, but along three of them both run the same way: the
    // surface has no consistent inside (mesh.hpp: "they run along it in opposite directions").
    Mesh cube = unit_cube();
    flip_triangle(cube, 4);
    CHECK_FALSE(is_closed(cube));
}

TEST_CASE("mesh: an inside-out cube is closed, with volume -1 m^3", "[geom][mesh]") {
    // Every triangle wound the other way faces inwards, as the inside of a room does.
    Mesh cube = unit_cube();
    for (std::size_t t = 0; t < triangle_count(cube); ++t) {
        flip_triangle(cube, t);
    }
    CHECK(is_closed(cube));
    CHECK_THAT(volume(cube), WithinAbs(-1.0, tight));
    CHECK_THAT(area(cube), WithinAbs(6.0, tight));
}

TEST_CASE("mesh: a mesh with no triangles is not closed and has no area", "[geom][mesh]") {
    SECTION("empty") {
        const Mesh empty;
        CHECK_FALSE(is_closed(empty));
        CHECK(area(empty) == 0.0);
    }
    SECTION("vertices but no triangles") {
        Mesh cube = unit_cube();
        cube.indices.clear();
        CHECK_FALSE(is_closed(cube));
        CHECK(area(cube) == 0.0);
    }
}

TEST_CASE("mesh: a mesh with an index out of range is not closed", "[geom][mesh]") {
    // Untrusted meshes must fail cleanly: no read past the positions (the sanitizers watch).
    Mesh cube = unit_cube();
    SECTION("one past the end") {
        cube.indices[5] = static_cast<std::uint32_t>(cube.positions.size());
        CHECK_FALSE(is_closed(cube));
    }
    SECTION("the largest index") {
        cube.indices[5] = std::numeric_limits<std::uint32_t>::max();
        CHECK_FALSE(is_closed(cube));
    }
    SECTION("positions missing") {
        cube.positions.resize(20);
        CHECK_FALSE(is_closed(cube));
    }
}

TEST_CASE("mesh: a mesh with a degenerate triangle is not closed", "[geom][mesh]") {
    // A closed cube plus one triangle with two equal corners C, C and a new vertex Q. Its edges
    // are C->C, C->Q and Q->C: the last two pair up with each other, and C->C is its own reverse,
    // so only the rule against degenerate triangles (mesh.hpp) makes this mesh not closed.
    Mesh cube = unit_cube();
    const std::uint32_t q = add_vertex(cube, {5.0, 5.0, 5.0});

    SECTION("the same index twice") {
        cube.indices.insert(cube.indices.end(), {0, 0, q});
        CHECK_FALSE(is_closed(cube));
    }
    SECTION("two indices at the same position") {
        // Vertex 0 is the corner (0, 0, 0) of the bottom face; the -y face has it too.
        std::uint32_t twin = 0;
        for (std::uint32_t i = 1; i < 24; ++i) {
            if (cube.positions[i] == cube.positions[0]) {
                twin = i;
                break;
            }
        }
        REQUIRE(twin != 0);
        cube.indices.insert(cube.indices.end(), {0, twin, q});
        CHECK_FALSE(is_closed(cube));
    }
    SECTION("all three corners the same") {
        cube.indices.insert(cube.indices.end(), {q, q, q});
        CHECK_FALSE(is_closed(cube));
    }
}

TEST_CASE("mesh: a mesh with a position that is not finite is not closed", "[geom][mesh]") {
    // Moving every vertex at one corner keeps the corners matching bit for bit, so only the rule
    // against positions that are not finite makes these not closed.
    Mesh cube = unit_cube();
    SECTION("infinity") {
        move_corner(cube, {1.0, 1.0, 1.0}, {infinity, 1.0, 1.0});
        CHECK_FALSE(is_closed(cube));
    }
    SECTION("minus infinity") {
        move_corner(cube, {1.0, 1.0, 1.0}, {1.0, 1.0, -infinity});
        CHECK_FALSE(is_closed(cube));
    }
    SECTION("NaN") {
        move_corner(cube, {1.0, 1.0, 1.0}, {1.0, not_a_number, 1.0});
        CHECK_FALSE(is_closed(cube));
    }
}

TEST_CASE("mesh: is_closed matches vertices by exact position, not by index", "[geom][mesh]") {
    SECTION("faces that share vertices by index are closed too") {
        // The cube with every corner's vertices merged into the first of them: 8 vertices used.
        Mesh cube = unit_cube();
        for (auto& index : cube.indices) {
            for (std::uint32_t first = 0; first < index; ++first) {
                if (cube.positions[first] == cube.positions[index]) {
                    index = first;
                    break;
                }
            }
        }
        CHECK(is_closed(cube));
        CHECK_THAT(volume(cube), WithinAbs(1.0, tight));
    }
    SECTION("a corner one picometre off on one face leaves a crack") {
        // Vertex 4 is the top face's corner (0, 0, 1); the -y and -x faces keep theirs.
        Mesh cube = unit_cube();
        REQUIRE(cube.positions[4] == dvec3(0.0, 0.0, 1.0));
        cube.positions[4].x += 1e-12;
        CHECK_FALSE(is_closed(cube));
    }
}

TEST_CASE("mesh: edges shared by more than two triangles are not closed", "[geom][mesh]") {
    // Two unit cubes side by side, touching along the face x = 1: each edge of that square has
    // four triangles on it, two from each cube.
    Mesh cubes = unit_cube();
    append(cubes, box({1.0, 0.0, 0.0}, {2.0, 1.0, 1.0}));
    CHECK_FALSE(is_closed(cubes));
}

TEST_CASE("mesh: volume and area do not depend on where the mesh is", "[geom][mesh]") {
    // A unit cube a kilometre or more from the origin, as a room of a large site may be, with
    // corners that are not exact in binary (1000.1 is not), so that its sides are 1 m to about
    // 1e-13 m and its volume 1 m^3 to about 1e-12. Measured from a point of the mesh, the volume
    // keeps that; a sum of tetrahedra from the origin loses about 1e-7 here to cancellation
    // (twelve terms of order 1000^3 / 6 adding up to 1). 1e-9 m^3 asks for the former. (Corners
    // at integers would not tell them apart: every product would be exact.)
    const dvec3 offset = GENERATE(dvec3(1000.1, 1000.2, 1000.3), dvec3(-1234.567, 678.9, 3000.01));
    INFO("offset " << Catch::StringMaker<dvec3>::convert(offset));
    const Mesh cube = box(offset, offset + dvec3(1.0));
    CHECK(is_closed(cube));
    CHECK_THAT(volume(cube), WithinAbs(1.0, 1e-9));
    CHECK_THAT(area(cube), WithinAbs(6.0, 1e-9));
}

TEST_CASE("mesh: append adds the vertices and offsets the indices", "[geom][mesh]") {
    // Two unit cubes a metre apart: together closed, with twice the area and volume.
    const Mesh a = unit_cube();
    const Mesh b = box({2.0, 0.0, 0.0}, {3.0, 1.0, 1.0});
    Mesh both = a;
    append(both, b);

    REQUIRE(both.positions.size() == 48);
    REQUIRE(both.normals.size() == 48);
    REQUIRE(both.uvs.size() == 48);
    REQUIRE(both.indices.size() == 72);
    for (std::size_t i = 0; i < 24; ++i) {
        CHECK(both.positions[i] == a.positions[i]);
        CHECK(both.positions[24 + i] == b.positions[i]);
        CHECK(both.normals[24 + i] == b.normals[i]);
        CHECK(both.uvs[24 + i] == b.uvs[i]);
    }
    for (std::size_t i = 0; i < 36; ++i) {
        CHECK(both.indices[i] == a.indices[i]);
        CHECK(both.indices[36 + i] == b.indices[i] + 24u);
    }

    check_mesh_conventions(both);
    CHECK(is_closed(both));
    CHECK_THAT(volume(both), WithinAbs(2.0, tight));
    CHECK_THAT(area(both), WithinAbs(12.0, tight));
}

TEST_CASE("mesh: append with an empty mesh", "[geom][mesh]") {
    const Mesh cube = unit_cube();
    SECTION("to an empty mesh copies") {
        Mesh to;
        append(to, cube);
        CHECK(to.positions == cube.positions);
        CHECK(to.normals == cube.normals);
        CHECK(to.uvs == cube.uvs);
        CHECK(to.indices == cube.indices);
    }
    SECTION("of an empty mesh changes nothing") {
        Mesh to = cube;
        append(to, Mesh{});
        CHECK(to.positions == cube.positions);
        CHECK(to.normals == cube.normals);
        CHECK(to.uvs == cube.uvs);
        CHECK(to.indices == cube.indices);
    }
}
