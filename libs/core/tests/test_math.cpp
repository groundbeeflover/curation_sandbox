// SPDX-License-Identifier: Apache-2.0

#include <cs/core/math.hpp>
#include <cs/core/random.hpp>
#include <cs/core/sampling.hpp>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <vector>

using Catch::Matchers::WithinAbs;

// --- Criterion 5a: the aliases are glm's types --------------------------------------------------

static_assert(std::is_same_v<cs::Vec2f, glm::vec2>);
static_assert(std::is_same_v<cs::Vec3f, glm::vec3>);
static_assert(std::is_same_v<cs::Vec4f, glm::vec4>);
static_assert(std::is_same_v<cs::Mat3f, glm::mat3>);
static_assert(std::is_same_v<cs::Mat4f, glm::mat4>);
static_assert(std::is_same_v<cs::Vec2d, glm::dvec2>);
static_assert(std::is_same_v<cs::Vec3d, glm::dvec3>);
static_assert(std::is_same_v<cs::Vec4d, glm::dvec4>);
static_assert(std::is_same_v<cs::Mat3d, glm::dmat3>);
static_assert(std::is_same_v<cs::Mat4d, glm::dmat4>);

// And the precisions are what their names say.
static_assert(std::is_same_v<cs::Vec3f::value_type, float>);
static_assert(std::is_same_v<cs::Vec3d::value_type, double>);
static_assert(std::is_same_v<cs::Mat4f::value_type, float>);

TEST_CASE("math.hpp provides Vec3f, Vec3d and Mat4f as the glm types", "[core][math]") {
    // The static_asserts above are the test; this case makes the criterion visible in the
    // test list and checks the types work as glm's do.
    const cs::Mat4f identity(1.0f);
    const cs::Vec4f p(1.0f, 2.0f, 3.0f, 1.0f);
    CHECK(identity * p == p);
    CHECK(glm::dot(cs::Vec3d(1.0, 2.0, 3.0), cs::Vec3d(4.0, 5.0, 6.0)) == 32.0);
}

// --- Criterion 5b: onb_from_normal is orthonormal ----------------------------------------------

namespace {

// The criterion asks for orthonormality within 1e-6. For double the method's own error is a few
// ulps (~1e-15), so 1e-6 is very loose but is the criterion's number. For float, unit length and
// orthogonality come out within a few float ulps of 1 (6e-8 each), and the normal itself, made
// in double and rounded to float, is unit only to ~1e-7; 1e-6 still holds with room to spare.
template <typename T> constexpr double onb_tolerance = 1e-6;

template <typename T> struct OnbErrors {
    double length = 0.0;     // | |t| - 1 |, | |b| - 1 |
    double orthogonal = 0.0; // |t.b|, |t.n|, |b.n|
    double handedness = 0.0; // max component of |t x b - n|
};

template <typename T> OnbErrors<T> onb_errors(const cs::Onb<T>& onb) {
    const glm::vec<3, T> c = glm::cross(onb.tangent, onb.bitangent) - onb.normal;
    OnbErrors<T> e;
    e.length = std::max(std::abs(double(glm::length(onb.tangent)) - 1.0),
                        std::abs(double(glm::length(onb.bitangent)) - 1.0));
    e.orthogonal = std::max({std::abs(double(glm::dot(onb.tangent, onb.bitangent))),
                             std::abs(double(glm::dot(onb.tangent, onb.normal))),
                             std::abs(double(glm::dot(onb.bitangent, onb.normal)))});
    e.handedness = std::max({std::abs(double(c.x)), std::abs(double(c.y)), std::abs(double(c.z))});
    return e;
}

template <typename T> void check_onb(const glm::vec<3, T>& normal) {
    INFO("normal (" << normal.x << ", " << normal.y << ", " << normal.z << ")");
    const cs::Onb<T> onb = cs::onb_from_normal(normal);
    // The normal is passed through untouched, bit for bit.
    CHECK(onb.normal == normal);
    CHECK(std::signbit(onb.normal.z) == std::signbit(normal.z));
    const OnbErrors<T> e = onb_errors(onb);
    CHECK(e.length <= onb_tolerance<T>);
    CHECK(e.orthogonal <= onb_tolerance<T>);
    CHECK(e.handedness <= onb_tolerance<T>);
}

template <typename T> std::vector<glm::vec<3, T>> random_unit_normals(int n, std::uint64_t seed) {
    cs::Rng rng(cs::hash_seed(seed, 0, 0, 0, 0));
    std::vector<glm::vec<3, T>> normals;
    normals.reserve(n);
    for (int i = 0; i < n; ++i) {
        const glm::dvec2 u(rng.next_double(), rng.next_double());
        // Unit in double; for float, rounded and renormalised in float, as a caller would.
        normals.push_back(glm::normalize(glm::vec<3, T>(cs::sample_uniform_sphere(u).direction)));
    }
    return normals;
}

} // namespace

TEMPLATE_TEST_CASE("onb_from_normal gives an orthonormal right-handed basis for 10^4 random "
                   "normals within 1e-6",
                   "[core][math][accuracy]", float, double) {
    using T = TestType;
    constexpr int n = 10'000;
    const std::vector<glm::vec<3, T>> normals = random_unit_normals<T>(n, 61);

    OnbErrors<T> worst;
    int changed_normals = 0;
    int below = 0;
    for (const glm::vec<3, T>& normal : normals) {
        const cs::Onb<T> onb = cs::onb_from_normal(normal);
        changed_normals += onb.normal == normal ? 0 : 1;
        below += normal.z < T(0) ? 1 : 0;
        const OnbErrors<T> e = onb_errors(onb);
        worst.length = std::max(worst.length, e.length);
        worst.orthogonal = std::max(worst.orthogonal, e.orthogonal);
        worst.handedness = std::max(worst.handedness, e.handedness);
    }
    INFO("worst length error " << worst.length << ", orthogonality " << worst.orthogonal
                               << ", handedness " << worst.handedness);
    CHECK(changed_normals == 0);
    CHECK(worst.length <= onb_tolerance<T>);
    CHECK(worst.orthogonal <= onb_tolerance<T>);
    CHECK(worst.handedness <= onb_tolerance<T>);

    // Both hemispheres were exercised (the method has a branch-free sign switch at z = 0):
    // about half are below; with n = 10^4 the standard deviation of the count is 50, so the
    // band 4000..6000 is 20 of them.
    CHECK(below > 4000);
    CHECK(below < 6000);
}

TEMPLATE_TEST_CASE("onb_from_normal at the axes, at -0 and near the poles", "[core][math]", float,
                   double) {
    using T = TestType;
    using V = glm::vec<3, T>;

    // The axes, where products vanish, and both poles: -z is where Frisvad's original method
    // divides by zero; the revisited one must not. (0, 0, -0) has its own test below.
    for (const V& normal :
         {V(1, 0, 0), V(-1, 0, 0), V(0, 1, 0), V(0, -1, 0), V(0, 0, 1), V(0, 0, -1)}) {
        check_onb(normal);
    }

    // Near the poles, where 1 / (sign + z) is largest.
    const T tiny = T(1e-4);
    for (const V& normal : {glm::normalize(V(tiny, tiny, 1)), glm::normalize(V(tiny, -tiny, -1)),
                            glm::normalize(V(-tiny, 0, -1)), glm::normalize(V(0, tiny, 1))}) {
        check_onb(normal);
    }

    // On the equator, either side of the jump at z = 0.
    for (const V& normal : {glm::normalize(V(1, 1, 0)), glm::normalize(V(-1, 1, T(-0.0))),
                            glm::normalize(V(1, 0, tiny)), glm::normalize(V(1, 0, -tiny))}) {
        check_onb(normal);
    }
}

TEMPLATE_TEST_CASE("onb_from_normal with a z of -0 takes the lower hemisphere and stays finite",
                   "[core][math]", float, double) {
    using T = TestType;
    using V = glm::vec<3, T>;
    // (0, 0, -0) is not a unit vector, so the basis need not be orthonormal around it, but the
    // sign of zero must not produce a division by zero: copysign gives -1, and -1 / (-1 + -0) = 1.
    // A unit normal on the equator with z = -0 is the case that matters, and is orthonormal.
    const cs::Onb<T> at_zero = cs::onb_from_normal(V(0, 0, T(-0.0)));
    CHECK(std::isfinite(at_zero.tangent.x));
    CHECK(std::isfinite(at_zero.tangent.y));
    CHECK(std::isfinite(at_zero.tangent.z));
    CHECK(std::isfinite(at_zero.bitangent.x));
    CHECK(std::isfinite(at_zero.bitangent.y));
    CHECK(std::isfinite(at_zero.bitangent.z));
    CHECK(std::signbit(at_zero.normal.z));

    check_onb(V(1, 0, T(-0.0)));
    check_onb(V(0, -1, T(-0.0)));
    check_onb(V(1, 0, T(0.0)));
}

TEMPLATE_TEST_CASE("Onb to_local and to_world round-trip and agree with the basis",
                   "[core][math][accuracy]", float, double) {
    using T = TestType;
    using V = glm::vec<3, T>;
    // to_local(to_world(v)) is B^T B v for a basis B orthonormal to ~1 ulp, so it returns v to a
    // few ulps of |v|: 1e-6 for float (|v| <= 2 here, float ulp 1.2e-7 at 1), 1e-12 for double.
    const double tolerance = std::is_same_v<T, float> ? 1e-6 : 1e-12;

    cs::Rng rng(cs::hash_seed(62, 0, 0, 0, 0));
    const std::vector<V> normals = random_unit_normals<T>(1000, 63);
    double worst_local = 0.0;
    double worst_world = 0.0;
    for (const V& normal : normals) {
        const cs::Onb<T> onb = cs::onb_from_normal(normal);
        // Any vector, not only unit ones, in [-1, 1)^3.
        const V v(T(2 * rng.next_double() - 1), T(2 * rng.next_double() - 1),
                  T(2 * rng.next_double() - 1));
        const V back = onb.to_local(onb.to_world(v));
        const V forth = onb.to_world(onb.to_local(v));
        for (int i = 0; i < 3; ++i) {
            worst_local = std::max(worst_local, std::abs(double(back[i] - v[i])));
            worst_world = std::max(worst_world, std::abs(double(forth[i] - v[i])));
        }

        // The local axes go to the basis vectors and +z is the normal.
        CHECK(onb.to_world(V(0, 0, 1)) == normal);
        CHECK(onb.to_world(V(1, 0, 0)) == onb.tangent);
        CHECK(onb.to_world(V(0, 1, 0)) == onb.bitangent);
        const V n_local = onb.to_local(normal);
        CHECK_THAT(double(n_local.x), WithinAbs(0.0, tolerance));
        CHECK_THAT(double(n_local.y), WithinAbs(0.0, tolerance));
        CHECK_THAT(double(n_local.z), WithinAbs(1.0, tolerance));
    }
    INFO("worst round-trip error " << worst_local << " (local), " << worst_world << " (world)");
    CHECK(worst_local <= tolerance);
    CHECK(worst_world <= tolerance);
}
