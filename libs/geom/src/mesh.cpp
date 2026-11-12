// SPDX-License-Identifier: Apache-2.0

#include <cs/geom/mesh.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace cs::geom {

namespace {

// The corners of triangle t. Throws std::out_of_range for an index out of range.
std::array<glm::dvec3, 3> corners(const Mesh& mesh, std::size_t t) {
    return {mesh.positions.at(mesh.indices.at(3 * t)),
            mesh.positions.at(mesh.indices.at(3 * t + 1)),
            mesh.positions.at(mesh.indices.at(3 * t + 2))};
}

std::size_t triangle_count(const Mesh& mesh) {
    return mesh.indices.size() / 3;
}

// Orders positions so that exactly equal ones (0.0 and -0.0 included) are the same key.
struct PositionLess {
    bool operator()(const glm::dvec3& a, const glm::dvec3& b) const {
        return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
    }
};

} // namespace

void append(Mesh& to, const Mesh& from) {
    const std::size_t offset = to.positions.size();
    if (from.positions.size() > std::numeric_limits<std::uint32_t>::max() - offset) {
        throw std::length_error("geom::append: more vertices than 32-bit indices can number");
    }
    to.positions.insert(to.positions.end(), from.positions.begin(), from.positions.end());
    to.normals.insert(to.normals.end(), from.normals.begin(), from.normals.end());
    to.uvs.insert(to.uvs.end(), from.uvs.begin(), from.uvs.end());
    to.indices.reserve(to.indices.size() + from.indices.size());
    for (const std::uint32_t index : from.indices) {
        to.indices.push_back(static_cast<std::uint32_t>(offset + index));
    }
}

double area(const Mesh& mesh) {
    double sum = 0.0;
    for (std::size_t t = 0; t < triangle_count(mesh); ++t) {
        const auto [a, b, c] = corners(mesh, t);
        sum += 0.5 * glm::length(glm::cross(b - a, c - a));
    }
    return sum;
}

double volume(const Mesh& mesh) {
    if (mesh.positions.empty()) {
        return 0.0;
    }
    // The divergence theorem: the sum of the signed volumes of the tetrahedra from a reference
    // point to each triangle. Any reference point gives the same sum for a closed mesh; one on
    // the mesh keeps the terms small when the mesh is far from the origin.
    const glm::dvec3 reference = mesh.positions.front();
    double six_times = 0.0;
    for (std::size_t t = 0; t < triangle_count(mesh); ++t) {
        const auto [a, b, c] = corners(mesh, t);
        six_times += glm::dot(a - reference, glm::cross(b - reference, c - reference));
    }
    return six_times / 6.0;
}

bool is_closed(const Mesh& mesh) {
    if (mesh.indices.empty() || mesh.indices.size() % 3 != 0) {
        return false;
    }

    // Number the distinct points: vertices with exactly equal positions are one point.
    std::map<glm::dvec3, std::uint32_t, PositionLess> numbers;
    std::vector<std::uint32_t> point_of(mesh.positions.size());
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
        const glm::dvec3& p = mesh.positions[i];
        if (!(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z))) {
            return false;
        }
        const auto number = static_cast<std::uint32_t>(numbers.size());
        point_of[i] = numbers.try_emplace(p, number).first->second;
    }

    // Each triangle's edges, as pairs of points in the direction the triangle runs along them.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    edges.reserve(mesh.indices.size());
    for (std::size_t t = 0; t < triangle_count(mesh); ++t) {
        std::array<std::uint32_t, 3> p{};
        for (std::size_t k = 0; k < 3; ++k) {
            const std::uint32_t index = mesh.indices[3 * t + k];
            if (index >= mesh.positions.size()) {
                return false;
            }
            p[k] = point_of[index];
        }
        if (p[0] == p[1] || p[1] == p[2] || p[2] == p[0]) {
            return false;
        }
        edges.emplace_back(p[0], p[1]);
        edges.emplace_back(p[1], p[2]);
        edges.emplace_back(p[2], p[0]);
    }

    // Closed: each directed edge appears once, and so does its reverse.
    std::sort(edges.begin(), edges.end());
    if (std::adjacent_find(edges.begin(), edges.end()) != edges.end()) {
        return false;
    }
    return std::all_of(edges.begin(), edges.end(), [&edges](const auto& edge) {
        return std::binary_search(edges.begin(), edges.end(), std::pair{edge.second, edge.first});
    });
}

} // namespace cs::geom
