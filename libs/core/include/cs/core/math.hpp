// SPDX-License-Identifier: Apache-2.0

#pragma once

// Vector types and orthonormal bases
// ----------------------------------
// The project's vectors and matrices are glm's; these names are the ones code outside glm-heavy
// maths uses. Single precision (f) is for the GPU and for images; double precision (d) is for the
// calculation engine, where lux has to add up over many bounces without drifting.

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cmath>

namespace cs {

using Vec2f = glm::vec2;
using Vec3f = glm::vec3;
using Vec4f = glm::vec4;
using Mat3f = glm::mat3;
using Mat4f = glm::mat4;

using Vec2d = glm::dvec2;
using Vec3d = glm::dvec3;
using Vec4d = glm::dvec4;
using Mat3d = glm::dmat3;
using Mat4d = glm::dmat4;

// A right-handed orthonormal basis (tangent, bitangent, normal): tangent x bitangent = normal.
// It turns directions between the world and a local frame in which +z is the normal, the frame
// that cs::sample_cosine_hemisphere and cs::material::Brdf work in.
template <typename T> struct Onb {
    glm::vec<3, T> tangent;
    glm::vec<3, T> bitangent;
    glm::vec<3, T> normal;

    // Local coordinates (x along the tangent, y the bitangent, z the normal) to world.
    glm::vec<3, T> to_world(const glm::vec<3, T>& local) const {
        return local.x * tangent + local.y * bitangent + local.z * normal;
    }

    // World direction to local coordinates.
    glm::vec<3, T> to_local(const glm::vec<3, T>& world) const {
        return {glm::dot(world, tangent), glm::dot(world, bitangent), glm::dot(world, normal)};
    }
};

// A basis around a unit normal, without branches or a square root: Duff, Burgess, Christensen,
// Hery, Kensler, Liani and Villemin, Building an Orthonormal Basis, Revisited (JCGT 6(1), 2017),
// listing 3. It stays accurate as the normal approaches -z, where Frisvad's original method
// breaks down. The tangent and bitangent are continuous over each hemisphere (z >= 0, z < 0) and
// jump where the normal crosses z = 0, which does not matter for sampling.
// The normal must be unit length; the basis is only as orthonormal as the normal is unit.
template <typename T> Onb<T> onb_from_normal(const glm::vec<3, T>& normal) {
    const T sign = std::copysign(T(1), normal.z);
    const T a = T(-1) / (sign + normal.z);
    const T b = normal.x * normal.y * a;
    return {
        .tangent = {T(1) + sign * normal.x * normal.x * a, sign * b, -sign * normal.x},
        .bitangent = {b, sign + normal.y * normal.y * a, -normal.y},
        .normal = normal,
    };
}

} // namespace cs
