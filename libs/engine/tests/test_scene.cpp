// SPDX-License-Identifier: Apache-2.0

// The engine's ray-traced scene (scene.hpp), from issue #68 (M1-15): "An Embree scene built from
// a World answers ray queries with the hit instance, distance and normal". The world here is
// built by hand: one square mesh used by three instances (two with translations, one turned on
// its side), and a triangle mesh used by a fourth, so every expected hit is worked out on paper.

#include <cs/engine/scene.hpp>

#include <cs/core/thread_pool.hpp>
#include <cs/geom/mesh.hpp>
#include <cs/world/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using cs::engine::Hit;
using cs::engine::Scene;
using glm::dvec2;
using glm::dvec3;
using glm::dvec4;

namespace world = cs::world;
namespace geom = cs::geom;

namespace {

// Embree is single precision (scene.hpp): at the coordinates here, up to about 12 m from the
// origin, a float's spacing is 1e-6 m, and a hit distance is good to a few of those. 1e-5 m
// leaves a factor of ten and still tells apart any two surfaces of this world, which are at
// least 1 m apart.
constexpr double distance_tolerance_m = 1e-5;
// Normals are computed from float positions or interpolated from the mesh's: good to about
// 1e-6.
constexpr double normal_tolerance = 1e-5;
constexpr double inf = std::numeric_limits<double>::infinity();

void check_vec(const dvec3& actual, const dvec3& expected, double tolerance) {
    INFO("actual (" << actual.x << ", " << actual.y << ", " << actual.z << "), expected ("
                    << expected.x << ", " << expected.y << ", " << expected.z << ")");
    CHECK(std::abs(actual.x - expected.x) <= tolerance);
    CHECK(std::abs(actual.y - expected.y) <= tolerance);
    CHECK(std::abs(actual.z - expected.z) <= tolerance);
}

// The square [-1, 1]^2 at z = 0, facing +z, as two triangles wound counter-clockwise seen from
// +z: triangle 0 is (-1, -1), (1, -1), (1, 1), below the diagonal y = x; triangle 1 is (-1, -1),
// (1, 1), (-1, 1), above it. UVs map the square to [0, 1]^2: uv = ((x + 1) / 2, (y + 1) / 2).
geom::Mesh square() {
    geom::Mesh mesh;
    mesh.positions = {{-1.0, -1.0, 0.0}, {1.0, -1.0, 0.0}, {1.0, 1.0, 0.0}, {-1.0, 1.0, 0.0}};
    mesh.normals.assign(4, dvec3(0.0, 0.0, 1.0));
    mesh.uvs = {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

// One triangle in the plane x = 0 facing +x: (0, 0, 0), (0, 1, 0), (0, 0, 1), counter-clockwise
// seen from +x.
geom::Mesh triangle_facing_x() {
    geom::Mesh mesh;
    mesh.positions = {{0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
    mesh.normals.assign(3, dvec3(1.0, 0.0, 0.0));
    mesh.uvs = {{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}};
    mesh.indices = {0, 1, 2};
    return mesh;
}

glm::dmat4 translation(const dvec3& t) {
    glm::dmat4 m(1.0);
    m[3] = dvec4(t, 1.0);
    return m;
}

// A quarter turn about +x, then a translation: x stays x, y goes to +z and z to -y. So the
// square, facing +z, ends up upright in the plane y = t.y, facing -y.
glm::dmat4 quarter_turn_about_x_then(const dvec3& t) {
    glm::dmat4 m(1.0);
    m[0] = dvec4(1.0, 0.0, 0.0, 0.0);  // the image of x
    m[1] = dvec4(0.0, 0.0, 1.0, 0.0);  // the image of y
    m[2] = dvec4(0.0, -1.0, 0.0, 0.0); // the image of z
    m[3] = dvec4(t, 1.0);
    return m;
}

world::Instance instance(std::uint32_t mesh, std::uint32_t material, const glm::dmat4& transform) {
    world::Instance i;
    i.part = world::Part::room;
    i.mesh = mesh;
    i.material = material;
    i.transform = transform;
    return i;
}

// The test world:
//   instance 0: the square at z = 0 (identity), x and y in [-1, 1], facing +z;
//   instance 1: the square turned upright at y = 0, centred on (10, 0, 2): x in [9, 11], z in
//               [1, 3], facing -y;
//   instance 2: the square at z = 1 over instance 0, facing +z;
//   instance 3: the triangle, moved to x = -5, facing +x.
world::World test_world() {
    world::World w;
    w.meshes = {square(), triangle_facing_x()};
    w.materials = {world::LambertMaterial{dvec3(0.8)}, world::LambertMaterial{dvec3(0.2)}};
    w.instances = {
        instance(0, 0, glm::dmat4(1.0)),
        instance(0, 1, quarter_turn_about_x_then({10.0, 0.0, 2.0})),
        instance(0, 0, translation({0.0, 0.0, 1.0})),
        instance(1, 1, translation({-5.0, 0.0, 0.0})),
    };
    return w;
}

} // namespace

TEST_CASE("Scene: a ray names the instance it hits, with its distance, position and normals",
          "[engine][scene]") {
    const Scene scene(test_world());

    SECTION("straight down onto instance 2, the square at z = 1") {
        const dvec3 origin(0.5, -0.5, 5.0);
        const dvec3 direction(0.0, 0.0, -1.0);
        const auto hit = scene.intersect(origin, direction);
        REQUIRE(hit.has_value());
        CHECK(hit->instance == 2);
        CHECK(hit->triangle == 0); // (0.5, -0.5) lies below the diagonal y = x
        CHECK_THAT(hit->distance_m, WithinAbs(4.0, distance_tolerance_m));
        check_vec(hit->position_m, dvec3(0.5, -0.5, 1.0), distance_tolerance_m);
        check_vec(hit->geometric_normal, dvec3(0.0, 0.0, 1.0), normal_tolerance);
        check_vec(hit->normal, dvec3(0.0, 0.0, 1.0), normal_tolerance);
        // uv = ((x + 1) / 2, (y + 1) / 2) at the square's own (0.5, -0.5).
        CHECK_THAT(hit->uv.x, WithinAbs(0.75, normal_tolerance));
        CHECK_THAT(hit->uv.y, WithinAbs(0.25, normal_tolerance));
    }

    SECTION("in the other triangle of the square") {
        const auto hit = scene.intersect({-0.5, 0.5, 5.0}, {0.0, 0.0, -1.0});
        REQUIRE(hit.has_value());
        CHECK(hit->instance == 2);
        CHECK(hit->triangle == 1);
        CHECK_THAT(hit->uv.x, WithinAbs(0.25, normal_tolerance));
        CHECK_THAT(hit->uv.y, WithinAbs(0.75, normal_tolerance));
    }

    SECTION("along +y onto instance 1, turned upright by its transform") {
        // The square's own (0.5, -0.5, 0) goes to (10 + 0.5, 0, 2 - 0.5); its normal +z to -y.
        const dvec3 origin(10.5, -5.0, 1.5);
        const auto hit = scene.intersect(origin, {0.0, 1.0, 0.0});
        REQUIRE(hit.has_value());
        CHECK(hit->instance == 1);
        CHECK(hit->triangle == 0);
        CHECK_THAT(hit->distance_m, WithinAbs(5.0, distance_tolerance_m));
        check_vec(hit->position_m, dvec3(10.5, 0.0, 1.5), distance_tolerance_m);
        check_vec(hit->geometric_normal, dvec3(0.0, -1.0, 0.0), normal_tolerance);
        check_vec(hit->normal, dvec3(0.0, -1.0, 0.0), normal_tolerance);
        CHECK_THAT(hit->uv.x, WithinAbs(0.75, normal_tolerance));
        CHECK_THAT(hit->uv.y, WithinAbs(0.25, normal_tolerance));
    }

    SECTION("an oblique ray onto the triangle of instance 3, of the second mesh") {
        // From (-3, 1.25, 0.25) to (-5, 0.25, 0.25), on the plane x = -5 and inside the
        // triangle (y + z = 0.5 < 1): direction (-2, -1, 0) / sqrt(5), distance sqrt(5).
        const dvec3 origin(-3.0, 1.25, 0.25);
        const dvec3 target(-5.0, 0.25, 0.25);
        const dvec3 direction = glm::normalize(target - origin);
        const auto hit = scene.intersect(origin, direction);
        REQUIRE(hit.has_value());
        CHECK(hit->instance == 3);
        CHECK(hit->triangle == 0);
        CHECK_THAT(hit->distance_m, WithinAbs(std::sqrt(5.0), distance_tolerance_m));
        check_vec(hit->position_m, target, distance_tolerance_m);
        check_vec(hit->geometric_normal, dvec3(1.0, 0.0, 0.0), normal_tolerance);
        check_vec(hit->normal, dvec3(1.0, 0.0, 0.0), normal_tolerance);
    }
}

TEST_CASE("Scene: the hit position is the origin plus the distance along the ray",
          "[engine][scene]") {
    const Scene scene(test_world());
    const dvec3 origin(0.3, 0.2, 7.0);
    const dvec3 direction = glm::normalize(dvec3(0.05, -0.02, -1.0));
    const auto hit = scene.intersect(origin, direction);
    REQUIRE(hit.has_value());
    check_vec(hit->position_m, origin + hit->distance_m * direction, distance_tolerance_m);
    // It lies on instance 2's plane, z = 1.
    CHECK_THAT(hit->position_m.z, WithinAbs(1.0, distance_tolerance_m));
}

TEST_CASE("Scene: a triangle is hit from behind too, and its geometric normal still faces its "
          "front",
          "[engine][scene]") {
    const Scene scene(test_world());
    // From below the square at z = 0 (instance 0), going up: its back.
    const auto hit = scene.intersect({0.5, -0.5, -3.0}, {0.0, 0.0, 1.0});
    REQUIRE(hit.has_value());
    CHECK(hit->instance == 0);
    CHECK_THAT(hit->distance_m, WithinAbs(3.0, distance_tolerance_m));
    // geometric_normal is the triangle's front (counter-clockwise), +z, whichever side the ray
    // came from; normal is the mesh's, also +z.
    check_vec(hit->geometric_normal, dvec3(0.0, 0.0, 1.0), normal_tolerance);
    check_vec(hit->normal, dvec3(0.0, 0.0, 1.0), normal_tolerance);

    // And instance 1 from its back, the +y side, going along -y.
    const auto back = scene.intersect({10.5, 5.0, 1.5}, {0.0, -1.0, 0.0});
    REQUIRE(back.has_value());
    CHECK(back->instance == 1);
    CHECK_THAT(back->distance_m, WithinAbs(5.0, distance_tolerance_m));
    check_vec(back->geometric_normal, dvec3(0.0, -1.0, 0.0), normal_tolerance);
}

TEST_CASE("Scene: a ray that hits nothing returns no hit", "[engine][scene]") {
    const Scene scene(test_world());
    CHECK_FALSE(scene.intersect({0.0, 0.0, 5.0}, {0.0, 0.0, 1.0}).has_value());  // away from all
    CHECK_FALSE(scene.intersect({5.0, 5.0, 5.0}, {0.0, 0.0, -1.0}).has_value()); // beside all
    // Parallel to the square at z = 1, above it.
    CHECK_FALSE(scene.intersect({-3.0, 0.0, 1.5}, {1.0, 0.0, 0.0}).has_value());
}

TEST_CASE("Scene: intersect returns the closest hit with a distance in [t_min, t_max]",
          "[engine][scene]") {
    const Scene scene(test_world());
    const dvec3 origin(0.0, 0.0, 5.0);
    const dvec3 down(0.0, 0.0, -1.0);
    // Along this ray: instance 2 at distance 4, instance 0 at distance 5.

    SECTION("no limits: the nearer of the two") {
        const auto hit = scene.intersect(origin, down);
        REQUIRE(hit.has_value());
        CHECK(hit->instance == 2);
    }
    SECTION("t_min past the first: the second") {
        const auto hit = scene.intersect(origin, down, 4.5);
        REQUIRE(hit.has_value());
        CHECK(hit->instance == 0);
        CHECK_THAT(hit->distance_m, WithinAbs(5.0, distance_tolerance_m));
    }
    SECTION("t_max short of the first: nothing") {
        CHECK_FALSE(scene.intersect(origin, down, 0.0, 3.9).has_value());
    }
    SECTION("t_max between the two: the first") {
        const auto hit = scene.intersect(origin, down, 0.0, 4.5);
        REQUIRE(hit.has_value());
        CHECK(hit->instance == 2);
    }
    SECTION("an interval between the two: nothing") {
        CHECK_FALSE(scene.intersect(origin, down, 4.1, 4.9).has_value());
    }
    SECTION("t_min past both: nothing") {
        CHECK_FALSE(scene.intersect(origin, down, 5.5, inf).has_value());
    }
}

TEST_CASE("Scene: occluded tells whether anything lies along the ray in [t_min, t_max]",
          "[engine][scene]") {
    const Scene scene(test_world());
    const dvec3 origin(0.0, 0.0, 5.0);
    const dvec3 down(0.0, 0.0, -1.0);

    CHECK(scene.occluded(origin, down, 0.0, 10.0));
    CHECK(scene.occluded(origin, down, 0.0, inf));
    CHECK_FALSE(scene.occluded(origin, down, 0.0, 3.9));            // stops short of instance 2
    CHECK_FALSE(scene.occluded(origin, down, 4.1, 4.9));            // between the two squares
    CHECK(scene.occluded(origin, down, 4.1, 5.1));                  // reaches instance 0
    CHECK_FALSE(scene.occluded(origin, down, 5.1, inf));            // past both
    CHECK_FALSE(scene.occluded(origin, {0.0, 0.0, 1.0}, 0.0, inf)); // away from all
    // From behind counts as well: from below instance 0, upwards.
    CHECK(scene.occluded({0.0, 0.0, -1.0}, {0.0, 0.0, 1.0}, 0.0, 2.0));
}

TEST_CASE("Scene: occluded and intersect agree on many rays", "[engine][scene]") {
    // A fan of rays from above the squares, some hitting, some missing: occluded over [0, inf)
    // is true exactly when intersect finds a hit, and over [0, distance - 1 mm) false.
    const Scene scene(test_world());
    // Off the axes a little, so that no ray runs exactly along a triangle's edge.
    const dvec3 origin(0.013, -0.007, 6.0);
    int hits = 0;
    int misses = 0;
    for (int i = -10; i <= 10; ++i) {
        for (int j = -10; j <= 10; ++j) {
            const dvec3 direction = glm::normalize(dvec3(0.04 * i, 0.04 * j, -1.0));
            const auto hit = scene.intersect(origin, direction);
            INFO("ray " << i << ", " << j);
            CHECK(scene.occluded(origin, direction, 0.0, inf) == hit.has_value());
            if (hit) {
                ++hits;
                CHECK_FALSE(scene.occluded(origin, direction, 0.0, hit->distance_m - 1e-3));
            } else {
                ++misses;
            }
        }
    }
    // Rays up to 0.4 off vertical in x and y: those within about 0.2 reach the square at z = 1
    // (5 m down, so within 1 m of its centre), those beyond about 0.17 in x or y miss the square
    // at z = 0 as well (6 m down). Both kinds occur.
    CHECK(hits > 0);
    CHECK(misses > 0);
}

TEST_CASE("Scene: an empty world builds, and every ray misses", "[engine][scene]") {
    const Scene scene(world::World{});
    CHECK_FALSE(scene.intersect({0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}).has_value());
    CHECK_FALSE(scene.occluded({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, 0.0, inf));
    CHECK(scene.world().instances.empty());
}

TEST_CASE("Scene: world() returns the world it was built from, which it copied",
          "[engine][scene]") {
    auto source = std::make_unique<world::World>(test_world());
    const Scene scene(*source);
    source.reset(); // the scene does not refer to it afterwards

    const world::World& w = scene.world();
    REQUIRE(w.instances.size() == 4);
    CHECK(w.instances[1].mesh == 0);
    CHECK(w.instances[1].material == 1);
    CHECK(w.instances[3].mesh == 1);
    REQUIRE(w.materials.size() == 2);
    CHECK(std::holds_alternative<world::LambertMaterial>(w.materials[1]));

    // And it still answers queries.
    const auto hit = scene.intersect({0.5, -0.5, 5.0}, {0.0, 0.0, -1.0});
    REQUIRE(hit.has_value());
    CHECK(hit->instance == 2);
}

TEST_CASE("Scene: a moved scene answers as the original did", "[engine][scene]") {
    Scene original(test_world());
    Scene moved(std::move(original));
    const auto hit = moved.intersect({10.5, -5.0, 1.5}, {0.0, 1.0, 0.0});
    REQUIRE(hit.has_value());
    CHECK(hit->instance == 1);

    Scene assigned(world::World{});
    assigned = std::move(moved);
    const auto again = assigned.intersect({0.5, -0.5, 5.0}, {0.0, 0.0, -1.0});
    REQUIRE(again.has_value());
    CHECK(again->instance == 2);
}

TEST_CASE("Scene: any number of threads may query it at once, with the same answers",
          "[engine][scene]") {
    const Scene scene(test_world());
    constexpr std::size_t rays = 4096;
    auto ray = [](std::size_t k) {
        const double i = static_cast<double>(k % 64) - 32.0;
        const double j = static_cast<double>(k / 64) - 32.0;
        return glm::normalize(dvec3(0.03 * i, 0.03 * j, -1.0));
    };
    const dvec3 origin(0.1, -0.2, 4.0);

    std::vector<double> serial(rays, -1.0);
    for (std::size_t k = 0; k < rays; ++k) {
        const auto hit = scene.intersect(origin, ray(k));
        serial[k] = hit ? hit->distance_m : -1.0;
    }

    cs::ThreadPool pool(8);
    std::vector<double> parallel(rays, -2.0);
    pool.parallel_for(rays, 32, [&](std::size_t begin, std::size_t end) {
        for (std::size_t k = begin; k < end; ++k) {
            const auto hit = scene.intersect(origin, ray(k));
            parallel[k] = hit ? hit->distance_m : -1.0;
        }
    });
    CHECK(parallel == serial);
}

TEST_CASE("Scene: an invalid world throws std::invalid_argument", "[engine][scene]") {
    world::World w = test_world();

    SECTION("an instance names a mesh that is not in the world") {
        w.instances[1].mesh = 2;
    }
    SECTION("an instance names a mesh far out of range") {
        w.instances[0].mesh = std::numeric_limits<std::uint32_t>::max();
    }
    SECTION("an instance names a material that is not in the world") {
        w.instances[2].material = 2;
    }
    SECTION("a mesh has an index out of range") {
        w.meshes[0].indices[4] = 4; // the square has positions 0 to 3
    }
    SECTION("a mesh has a huge index") {
        w.meshes[1].indices[2] = std::numeric_limits<std::uint32_t>::max();
    }
    SECTION("a mesh has a position that is not finite") {
        w.meshes[0].positions[2].x = std::numeric_limits<double>::quiet_NaN();
    }
    SECTION("a mesh has an infinite position") {
        w.meshes[1].positions[0].z = inf;
    }
    SECTION("a mesh has fewer normals than positions") {
        w.meshes[0].normals.pop_back();
    }
    SECTION("a mesh has more uvs than positions") {
        w.meshes[1].uvs.push_back(dvec2(0.0));
    }
    SECTION("a mesh has fewer uvs than positions") {
        w.meshes[0].uvs.clear();
    }

    CHECK_THROWS_AS(Scene(w), std::invalid_argument);
}

TEST_CASE("Scene: an instance with no materials in the world throws std::invalid_argument",
          "[engine][scene]") {
    world::World w = test_world();
    w.materials.clear();
    CHECK_THROWS_AS(Scene(w), std::invalid_argument);
}

TEST_CASE("Scene: a mesh that no instance uses does not change the hits", "[engine][scene]") {
    // A third mesh, valid, that no instance names: the scene builds and is unchanged.
    world::World w = test_world();
    w.meshes.push_back(square());
    const Scene scene(w);
    const auto hit = scene.intersect({0.5, -0.5, 5.0}, {0.0, 0.0, -1.0});
    REQUIRE(hit.has_value());
    CHECK(hit->instance == 2);
}

TEST_CASE("ray_offset_m: 1e-5 times the largest coordinate, and at least 1e-5 m",
          "[engine][scene]") {
    // scene.hpp: "1e-5 times the point's largest coordinate, and at least 1e-5 m", read as the
    // largest coordinate in magnitude, since a point at x = -10 is as far from the origin as one
    // at x = 10. Products of a few doubles: 1e-12 relative is far above rounding.
    CHECK_THAT(cs::engine::ray_offset_m({0.0, 0.0, 0.0}), WithinRel(1e-5, 1e-12));
    CHECK_THAT(cs::engine::ray_offset_m({0.5, 0.5, 0.5}), WithinRel(1e-5, 1e-12));
    CHECK_THAT(cs::engine::ray_offset_m({1.0, 1.0, 1.0}), WithinRel(1e-5, 1e-12));
    CHECK_THAT(cs::engine::ray_offset_m({10.0, 0.0, 0.0}), WithinRel(1e-4, 1e-12));
    CHECK_THAT(cs::engine::ray_offset_m({3.0, 4.0, 3.5}), WithinRel(4e-5, 1e-12));
    CHECK_THAT(cs::engine::ray_offset_m({1.0, 2.0, 20.0}), WithinRel(2e-4, 1e-12));
    CHECK_THAT(cs::engine::ray_offset_m({-10.0, 1.0, 1.0}), WithinRel(1e-4, 1e-12));
    CHECK_THAT(cs::engine::ray_offset_m({0.0, -1000.0, 0.0}), WithinRel(1e-2, 1e-12));
}

TEST_CASE("ray_offset_m: a ray that leaves a surface by its offset does not hit that surface",
          "[engine][scene]") {
    // Points on the square at z = 1 (instance 2), leaving upwards from ray_offset_m above it,
    // at grazing and steep angles: nothing is above, so nothing is hit.
    const Scene scene(test_world());
    for (const double x : {-0.9, -0.3, 0.0, 0.4, 0.99}) {
        for (const double slope : {0.01, 0.1, 1.0, 100.0}) {
            const dvec3 point(x, 0.25, 1.0);
            const dvec3 start = point + dvec3(0.0, 0.0, cs::engine::ray_offset_m(point));
            const dvec3 direction = glm::normalize(dvec3(1.0, 0.3, slope));
            INFO("x " << x << ", slope " << slope);
            CHECK_FALSE(scene.intersect(start, direction).has_value());
        }
    }
}
