// SPDX-License-Identifier: Apache-2.0

#pragma once

// What the world tests share: the skeleton scene (tests/scenes/skeleton/) and its ids, documents
// built in code that the loader is known to accept, a matcher for vectors, and measurements of a
// world's instances in world coordinates. Expected values stay in the tests.

#include <cs/world/compile.hpp>
#include <cs/world/ids.hpp>
#include <cs/world/world.hpp>

#include <cs/doc/components.hpp>
#include <cs/doc/document.hpp>
#include <cs/doc/project_io.hpp>
#include <cs/doc/uuid.hpp>
#include <cs/doc/work.hpp>
#include <cs/geom/mesh.hpp>
#include <cs/geom/wall_face.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Catch {

template <> struct StringMaker<glm::dvec2> {
    static std::string convert(const glm::dvec2& v) {
        std::ostringstream out;
        out.precision(17);
        out << '(' << v.x << ", " << v.y << ')';
        return out.str();
    }
};

template <> struct StringMaker<glm::dvec3> {
    static std::string convert(const glm::dvec3& v) {
        std::ostringstream out;
        out.precision(17);
        out << '(' << v.x << ", " << v.y << ", " << v.z << ')';
        return out.str();
    }
};

template <> struct StringMaker<cs::world::Id> {
    static std::string convert(const cs::world::Id& id) { return id.to_string(); }
};

} // namespace Catch

namespace cs::world::test {

constexpr double degrees = std::numbers::pi / 180.0;

// The issue's tolerance for the painting's corners and the exhibit's visible area: 0.1 mm.
inline constexpr double issue_tolerance_m = 1e-4;

// Positions here are of order 1 to 10 m, computed from the document by a few additions,
// multiplications and one rotation, so they are exact to about 1e-15 m. 1e-9 m leaves room for
// any order of computation while catching every real mistake, which is at least a millimetre.
inline constexpr double tolerance_m = 1e-9;

// A unit vector computed in double precision is unit to a few units in the last place (1e-16);
// 1e-12 allows for any order of computation and still catches one that was never normalised.
inline constexpr double unit_tolerance = 1e-12;

// ---- Vectors ---------------------------------------------------------------------------------

// Whether every component of a vector is within a tolerance of the expected one; NaN never is.
template <typename Vec> class VecWithinMatcher final : public Catch::Matchers::MatcherBase<Vec> {
  public:
    VecWithinMatcher(const Vec& expected, double tolerance)
        : expected_(expected), tolerance_(tolerance) {}

    bool match(const Vec& actual) const override {
        for (glm::length_t i = 0; i < Vec::length(); ++i) {
            if (!(std::abs(actual[i] - expected_[i]) <= tolerance_)) {
                return false;
            }
        }
        return true;
    }

    std::string describe() const override {
        return "is within " + Catch::StringMaker<double>::convert(tolerance_) + " of " +
               Catch::StringMaker<Vec>::convert(expected_);
    }

  private:
    Vec expected_;
    double tolerance_;
};

inline VecWithinMatcher<glm::dvec2> VecWithin(const glm::dvec2& expected, double tolerance) {
    return {expected, tolerance};
}

inline VecWithinMatcher<glm::dvec3> VecWithin(const glm::dvec3& expected, double tolerance) {
    return {expected, tolerance};
}

inline bool near(const glm::dvec3& a, const glm::dvec3& b, double tolerance) {
    return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance &&
           std::abs(a.z - b.z) <= tolerance;
}

// ---- The skeleton scene and documents --------------------------------------------------------

inline std::filesystem::path skeleton_folder() {
    return std::filesystem::path(CS_SOURCE_DIR) / "tests/scenes/skeleton";
}

// The skeleton's ids (tests/scenes/skeleton/project.json).
inline constexpr std::string_view chart_work_text = "01a1cd0d-dc00-7000-8000-000000000001";
inline constexpr std::string_view gallery_text = "01a1cd0d-dc00-7000-8000-000000000002";
inline constexpr std::string_view wall_a_text = "01a1cd0d-dc00-7000-8000-000000000003";
inline constexpr std::string_view wall_c_text = "01a1cd0d-dc00-7000-8000-000000000005";
inline constexpr std::string_view chart_text = "01a1cd0d-dc00-7000-8000-000000000007";
inline constexpr std::string_view track_text = "01a1cd0d-dc00-7000-8000-000000000008";
inline constexpr std::string_view spot_text = "01a1cd0d-dc00-7000-8000-000000000009";
inline constexpr std::string_view viewpoint_text = "01a1cd0d-dc00-7000-8000-00000000000a";

inline constexpr std::string_view chart_sha256 =
    "26d3b28857e3da2b25625116fcf14d505ad4ff83944c83cdb74759ad56f23b6a";

inline doc::Uuid uuid(std::string_view text) {
    const auto parsed = doc::Uuid::parse(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

inline Id id(std::string_view text) {
    return Id(uuid(text).bytes());
}

inline doc::Document skeleton() {
    return doc::load_project_folder(skeleton_folder());
}

inline doc::DocumentData skeleton_data() {
    return skeleton().data();
}

// The document `data` makes, after a round trip through project.json: the loader's checks have
// accepted it (parse_project_json throws otherwise), so it is a document compile must never throw
// for (compile.hpp).
inline doc::Document accepted(const doc::DocumentData& data) {
    return doc::parse_project_json(doc::to_project_json(doc::Document(data)));
}

inline doc::Entity& entity(doc::DocumentData& data, std::string_view text) {
    const auto it = data.entities.find(uuid(text));
    REQUIRE(it != data.entities.end());
    return it->second;
}

inline doc::Work& work(doc::DocumentData& data, std::string_view text) {
    const auto it = data.works.find(uuid(text));
    REQUIRE(it != data.works.end());
    return it->second;
}

// Face A of the skeleton's wall A: the wall from (0, -0.05) to (6, -0.05), 0.1 m thick, so the face
// lies on y = 0, faces +y into the room, and runs from its left end at x = 6 towards x = 0.
inline geom::FaceFrame skeleton_face_a() {
    return geom::wall_face_frame({0.0, -0.05}, {6.0, -0.05}, 0.1, geom::WallFace::a);
}

// ---- Instances in world coordinates ----------------------------------------------------------

inline glm::dvec3 point_to_world(const glm::dmat4& transform, const glm::dvec3& p) {
    return glm::dvec3(transform * glm::dvec4(p, 1.0));
}

inline glm::dvec3 direction_to_world(const glm::dmat4& transform, const glm::dvec3& d) {
    return glm::dvec3(transform * glm::dvec4(d, 0.0));
}

// The instance's mesh with its positions and normals taken to the world by its transform (a
// rotation and a translation, so normals map as directions).
inline geom::Mesh in_world(const World& world, const Instance& instance) {
    REQUIRE(instance.mesh < world.meshes.size());
    geom::Mesh mesh = world.meshes[instance.mesh];
    for (auto& p : mesh.positions) {
        p = point_to_world(instance.transform, p);
    }
    for (auto& n : mesh.normals) {
        n = direction_to_world(instance.transform, n);
    }
    return mesh;
}

struct Box {
    glm::dvec3 min{std::numeric_limits<double>::infinity()};
    glm::dvec3 max{-std::numeric_limits<double>::infinity()};
};

inline Box bounds(const geom::Mesh& mesh) {
    Box box;
    for (const auto& p : mesh.positions) {
        box.min = glm::min(box.min, p);
        box.max = glm::max(box.max, p);
    }
    return box;
}

// Every instance of the entity that draws the part, in the world's order.
inline std::vector<std::size_t> instances_of(const World& world, Id entity, Part part) {
    std::vector<std::size_t> found;
    for (std::size_t i = 0; i < world.instances.size(); ++i) {
        if (world.instances[i].entity == entity && world.instances[i].part == part) {
            found.push_back(i);
        }
    }
    return found;
}

// The one instance of the entity that draws the part.
inline const Instance& the_instance(const World& world, Id entity, Part part) {
    const auto found = instances_of(world, entity, part);
    INFO("entity " << entity.to_string() << ", part " << static_cast<int>(part));
    REQUIRE(found.size() == 1);
    return world.instances[found.front()];
}

inline const Material& material_of(const World& world, const Instance& instance) {
    REQUIRE(instance.material < world.materials.size());
    return world.materials[instance.material];
}

inline const Exhibit& the_exhibit(const World& world, Id entity) {
    const Exhibit* found = nullptr;
    std::size_t count = 0;
    for (const auto& exhibit : world.exhibits) {
        if (exhibit.entity == entity) {
            found = &exhibit;
            ++count;
        }
    }
    INFO("exhibit " << entity.to_string());
    REQUIRE(count == 1);
    return *found;
}

inline const Light& the_light(const World& world, Id entity) {
    const Light* found = nullptr;
    std::size_t count = 0;
    for (const auto& light : world.lights) {
        if (light.entity == entity) {
            found = &light;
            ++count;
        }
    }
    INFO("light " << entity.to_string());
    REQUIRE(count == 1);
    return *found;
}

// A rotation and a translation (world.hpp): the upper 3x3 is orthonormal with determinant +1, and
// the bottom row is (0, 0, 0, 1). glm matrices are column-major: m[column][row].
inline void check_rigid(const glm::dmat4& transform) {
    const glm::dmat3 r(transform);
    const glm::dmat3 rtr = glm::transpose(r) * r;
    for (int c = 0; c < 3; ++c) {
        for (int row = 0; row < 3; ++row) {
            CHECK(std::abs(rtr[c][row] - (c == row ? 1.0 : 0.0)) <= unit_tolerance);
        }
        CHECK(transform[c][3] == 0.0);
    }
    CHECK(std::abs(glm::determinant(r) - 1.0) <= unit_tolerance);
    CHECK(transform[3][3] == 1.0);
}

// What every world holds together: meshes with one normal and uv per position and indices in
// range; instances whose mesh and material exist and whose transforms are rigid; exhibits whose
// instances exist, belong to them and draw the right parts.
inline void check_well_formed(const World& world) {
    for (std::size_t m = 0; m < world.meshes.size(); ++m) {
        const auto& mesh = world.meshes[m];
        INFO("mesh " << m);
        CHECK(mesh.normals.size() == mesh.positions.size());
        CHECK(mesh.uvs.size() == mesh.positions.size());
        CHECK(mesh.indices.size() % 3 == 0);
        std::size_t out_of_range = 0;
        for (const auto i : mesh.indices) {
            if (i >= mesh.positions.size()) {
                ++out_of_range;
            }
        }
        CHECK(out_of_range == 0);
    }
    for (std::size_t i = 0; i < world.instances.size(); ++i) {
        const auto& instance = world.instances[i];
        INFO("instance " << i);
        CHECK(instance.mesh < world.meshes.size());
        CHECK(instance.material < world.materials.size());
        check_rigid(instance.transform);
    }
    for (const auto& exhibit : world.exhibits) {
        INFO("exhibit " << exhibit.entity.to_string());
        REQUIRE(exhibit.painting_instance < world.instances.size());
        CHECK(world.instances[exhibit.painting_instance].entity == exhibit.entity);
        CHECK(world.instances[exhibit.painting_instance].part == Part::painting);
        if (exhibit.frame_instance) {
            REQUIRE(*exhibit.frame_instance < world.instances.size());
            CHECK(world.instances[*exhibit.frame_instance].entity == exhibit.entity);
            CHECK(world.instances[*exhibit.frame_instance].part == Part::frame);
        }
    }
    for (const auto& problem : world.problems) {
        CHECK_FALSE(problem.reason.empty());
    }
}

// Whether every position of `actual` lies within `tolerance` of some position of `expected`, and
// every position of `expected` within `tolerance` of some position of `actual`: the same corners,
// in any order.
inline bool same_corners(const std::vector<glm::dvec3>& actual,
                         const std::vector<glm::dvec3>& expected, double tolerance) {
    const auto covered = [tolerance](const std::vector<glm::dvec3>& points,
                                     const std::vector<glm::dvec3>& by) {
        for (const auto& p : points) {
            bool found = false;
            for (const auto& q : by) {
                if (near(p, q, tolerance)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return false;
            }
        }
        return true;
    };
    return covered(actual, expected) && covered(expected, actual);
}

} // namespace cs::world::test
