// SPDX-License-Identifier: Apache-2.0

#pragma once

// Checks the geom tests share: the conventions of mesh.hpp, which every mesh this module makes
// keeps, a matcher for vectors, and small measurements of meshes. Expected values stay in the
// tests.

#include <cs/geom/mesh.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cmath>
#include <cstddef>
#include <limits>
#include <sstream>
#include <string>

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

} // namespace Catch

namespace cs::geom::test {

// A normal is a unit vector computed in double precision: its length is 1 to a few units in the
// last place (about 1e-16). 1e-12 allows for any order of computation and still catches a normal
// that was never normalised.
inline constexpr double unit_tolerance = 1e-12;

// Positions, lengths and directions here are of order 1 to 10 m, computed from the inputs by a few
// additions and multiplications, so they are exact to about 1e-15 m. 1e-9 m (a nanometre, the
// issue's tolerance for the wall face) leaves room for any order of computation while catching
// every real mistake, which is at least a millimetre.
inline constexpr double tolerance_m = 1e-9;

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

inline bool is_finite(const glm::dvec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

inline bool is_finite(const glm::dvec2& v) {
    return std::isfinite(v.x) && std::isfinite(v.y);
}

inline std::size_t triangle_count(const Mesh& mesh) {
    return mesh.indices.size() / 3;
}

// One triangle's corners, copied out of the mesh. The caller makes sure the indices are in range.
struct Triangle {
    glm::dvec3 p[3];
    glm::dvec3 n[3];
    glm::dvec2 uv[3];
};

inline Triangle triangle(const Mesh& mesh, std::size_t t) {
    Triangle tri;
    for (std::size_t k = 0; k < 3; ++k) {
        const std::size_t i = mesh.indices[3 * t + k];
        tri.p[k] = mesh.positions[i];
        tri.n[k] = mesh.normals[i];
        tri.uv[k] = mesh.uvs[i];
    }
    return tri;
}

// The unit normal of the triangle's plane on the side from which its corners run
// counter-clockwise (the right-hand rule); zero for a degenerate triangle.
inline glm::dvec3 winding_normal(const Triangle& tri) {
    const glm::dvec3 c = glm::cross(tri.p[1] - tri.p[0], tri.p[2] - tri.p[0]);
    const double length = glm::length(c);
    return length > 0.0 ? c / length : glm::dvec3(0.0);
}

inline double triangle_area(const Triangle& tri) {
    return 0.5 * glm::length(glm::cross(tri.p[1] - tri.p[0], tri.p[2] - tri.p[0]));
}

inline glm::dvec3 centroid(const Triangle& tri) {
    return (tri.p[0] + tri.p[1] + tri.p[2]) / 3.0;
}

// How the position changes with each UV coordinate across a triangle: dP/du and dP/dv, from
// p1 - p0 = du * (uv1 - uv0).x + dv * (uv1 - uv0).y and the same for p2. `valid` is false when the
// UVs of the corners are collinear, so that no such axes exist.
struct UvAxes {
    glm::dvec3 du{0.0};
    glm::dvec3 dv{0.0};
    bool valid = false;
};

inline UvAxes uv_axes(const Triangle& tri) {
    const glm::dvec3 e1 = tri.p[1] - tri.p[0];
    const glm::dvec3 e2 = tri.p[2] - tri.p[0];
    const glm::dvec2 d1 = tri.uv[1] - tri.uv[0];
    const glm::dvec2 d2 = tri.uv[2] - tri.uv[0];
    const double det = d1.x * d2.y - d2.x * d1.y;
    if (!(std::abs(det) > 0.0)) {
        return {};
    }
    return {(e1 * d2.y - e2 * d1.y) / det, (e2 * d1.x - e1 * d2.x) / det, true};
}

// The conventions of mesh.hpp that every mesh this module makes keeps: at least one triangle; one
// normal and one uv per position; three indices per triangle, each in range; finite values; unit
// normals; and each triangle wound counter-clockwise seen from its front, the side its normals
// point to. The module's faces are flat, each with its own vertices, so every vertex normal of a
// triangle equals the normal of its winding: 1e-9 on the cosine is an angle of 4.5e-5 rad, far
// below any real disagreement (a flipped triangle gives -1).
inline void check_mesh_conventions(const Mesh& mesh) {
    REQUIRE_FALSE(mesh.indices.empty());
    REQUIRE(mesh.normals.size() == mesh.positions.size());
    REQUIRE(mesh.uvs.size() == mesh.positions.size());
    REQUIRE(mesh.indices.size() % 3 == 0);

    std::size_t out_of_range = 0;
    for (const auto i : mesh.indices) {
        if (i >= mesh.positions.size()) {
            ++out_of_range;
        }
    }
    REQUIRE(out_of_range == 0);

    std::size_t not_finite = 0;
    std::size_t not_unit = 0;
    std::size_t first_not_unit = 0;
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
        if (!is_finite(mesh.positions[i]) || !is_finite(mesh.normals[i]) ||
            !is_finite(mesh.uvs[i])) {
            ++not_finite;
        }
        if (!(std::abs(glm::length(mesh.normals[i]) - 1.0) <= unit_tolerance)) {
            if (not_unit == 0) {
                first_not_unit = i;
            }
            ++not_unit;
        }
    }
    CHECK(not_finite == 0);
    {
        INFO("first vertex whose normal is not unit length: " << first_not_unit);
        CHECK(not_unit == 0);
    }

    std::size_t degenerate = 0;
    std::size_t against = 0;
    std::size_t first_against = 0;
    for (std::size_t t = 0; t < triangle_count(mesh); ++t) {
        const Triangle tri = triangle(mesh, t);
        const glm::dvec3 wn = winding_normal(tri);
        if (wn == glm::dvec3(0.0)) {
            ++degenerate;
            continue;
        }
        for (const glm::dvec3& n : tri.n) {
            if (!(glm::dot(wn, n) >= 1.0 - 1e-9)) {
                if (against == 0) {
                    first_against = t;
                }
                ++against;
                break;
            }
        }
    }
    CHECK(degenerate == 0);
    {
        INFO("first triangle whose winding disagrees with its normals: " << first_against);
        CHECK(against == 0);
    }
}

// UVs in metres (mesh.hpp): across every triangle, a step of 1 in u or v is a step of 1 m along
// the surface, the two at right angles, and u x v is the triangle's normal, so that a texture reads
// unmirrored from the front. Tolerance as tolerance_m: the axes come from differences of positions
// and UVs of order metres.
inline void check_uvs_in_metres(const Mesh& mesh) {
    std::size_t not_metres = 0;
    std::size_t first_not_metres = 0;
    for (std::size_t t = 0; t < triangle_count(mesh); ++t) {
        const Triangle tri = triangle(mesh, t);
        const UvAxes axes = uv_axes(tri);
        const bool metres = axes.valid && std::abs(glm::length(axes.du) - 1.0) <= tolerance_m &&
                            std::abs(glm::length(axes.dv) - 1.0) <= tolerance_m &&
                            std::abs(glm::dot(axes.du, axes.dv)) <= tolerance_m &&
                            glm::length(glm::cross(axes.du, axes.dv) - tri.n[0]) <= tolerance_m;
        if (!metres) {
            if (not_metres == 0) {
                first_not_metres = t;
            }
            ++not_metres;
        }
    }
    INFO("first triangle whose UVs are not metres along it, right-handed with its normal: "
         << first_not_metres);
    CHECK(not_metres == 0);
}

// The smallest box around a mesh's positions.
struct Bounds {
    glm::dvec3 lo{std::numeric_limits<double>::infinity()};
    glm::dvec3 hi{-std::numeric_limits<double>::infinity()};
};

inline Bounds bounds(const Mesh& mesh) {
    Bounds b;
    for (const glm::dvec3& p : mesh.positions) {
        for (glm::length_t k = 0; k < 3; ++k) {
            b.lo[k] = p[k] < b.lo[k] ? p[k] : b.lo[k];
            b.hi[k] = p[k] > b.hi[k] ? p[k] : b.hi[k];
        }
    }
    return b;
}

} // namespace cs::geom::test
