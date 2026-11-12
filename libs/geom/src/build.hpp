// SPDX-License-Identifier: Apache-2.0

#pragma once

// What the generators share: checking their arguments, and adding flat quads to a mesh.

#include <cs/geom/mesh.hpp>

#include <glm/geometric.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace cs::geom::detail {

// Throws std::invalid_argument: "geom::<function>: <name> must be <must_be>, not <value>".
[[noreturn]] inline void reject(const char* function, const char* name, const char* must_be,
                                double value) {
    throw std::invalid_argument(std::string("geom::") + function + ": " + name + " must be " +
                                must_be + ", not " + std::to_string(value));
}

// Each rejects the value, naming the function and the argument, unless it is finite and as the
// name says.
inline void require_finite(double value, const char* function, const char* name) {
    if (!std::isfinite(value)) {
        reject(function, name, "finite", value);
    }
}

inline void require_positive(double value, const char* function, const char* name) {
    if (!(std::isfinite(value) && value > 0.0)) {
        reject(function, name, "positive and finite", value);
    }
}

inline void require_non_negative(double value, const char* function, const char* name) {
    if (!(std::isfinite(value) && value >= 0.0)) {
        reject(function, name, "at least 0 and finite", value);
    }
}

// Adds a flat quad: four corners anticlockwise seen from the front, the side `normal` points to,
// with their UVs, as two triangles.
inline void add_quad(Mesh& mesh, const std::array<glm::dvec3, 4>& corners, glm::dvec3 normal,
                     const std::array<glm::dvec2, 4>& uvs) {
    const auto first = static_cast<std::uint32_t>(mesh.positions.size());
    for (std::size_t i = 0; i < 4; ++i) {
        mesh.positions.push_back(corners[i]);
        mesh.normals.push_back(normal);
        mesh.uvs.push_back(uvs[i]);
    }
    for (const std::uint32_t corner : {0u, 1u, 2u, 0u, 2u, 3u}) {
        mesh.indices.push_back(first + corner);
    }
}

// The same, with UVs in metres: each corner's position less uv_origin, measured along u_axis and
// v_axis, which are unit, at right angles and in the quad's plane. The normal is
// cross(u_axis, v_axis), as mesh.hpp's conventions say.
inline void add_planar_quad(Mesh& mesh, const std::array<glm::dvec3, 4>& corners, glm::dvec3 u_axis,
                            glm::dvec3 v_axis, glm::dvec3 uv_origin) {
    std::array<glm::dvec2, 4> uvs;
    for (std::size_t i = 0; i < 4; ++i) {
        const glm::dvec3 offset = corners[i] - uv_origin;
        uvs[i] = {glm::dot(offset, u_axis), glm::dot(offset, v_axis)};
    }
    add_quad(mesh, corners, glm::cross(u_axis, v_axis), uvs);
}

} // namespace cs::geom::detail
