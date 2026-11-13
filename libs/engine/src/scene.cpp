// SPDX-License-Identifier: Apache-2.0

#include <cs/engine/scene.hpp>

#include <embree4/rtcore.h>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cs::engine {

namespace {

bool finite(glm::dvec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// The positions and normals of one instance's mesh, in world coordinates; its indices and uvs
// are the world mesh's own.
struct WorldMesh {
    std::vector<glm::dvec3> positions;
    std::vector<glm::dvec3> normals;
};

WorldMesh to_world(const world::World& world, std::size_t instance_index) {
    const world::Instance& instance = world.instances[instance_index];
    const std::string where = "Scene: instance " + std::to_string(instance_index);
    if (instance.mesh >= world.meshes.size()) {
        throw std::invalid_argument(where + " names mesh " + std::to_string(instance.mesh) +
                                    ", which the world does not have");
    }
    if (instance.material >= world.materials.size()) {
        throw std::invalid_argument(where + " names material " + std::to_string(instance.material) +
                                    ", which the world does not have");
    }
    const geom::Mesh& mesh = world.meshes[instance.mesh];
    if (mesh.normals.size() != mesh.positions.size() || mesh.uvs.size() != mesh.positions.size()) {
        throw std::invalid_argument(where + ": its mesh has not one normal and uv per position");
    }
    if (mesh.indices.size() % 3 != 0) {
        throw std::invalid_argument(where + ": its mesh's index count is not a multiple of 3");
    }
    for (const std::uint32_t index : mesh.indices) {
        if (index >= mesh.positions.size()) {
            throw std::invalid_argument(where + ": its mesh has an index out of range");
        }
    }

    const glm::dmat4& m = instance.transform;
    const glm::dmat3 normal_matrix = glm::transpose(glm::inverse(glm::dmat3(m)));
    WorldMesh out;
    out.positions.reserve(mesh.positions.size());
    out.normals.reserve(mesh.normals.size());
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
        const glm::dvec3 p = glm::dvec3(m * glm::dvec4(mesh.positions[i], 1.0));
        if (!finite(p)) {
            throw std::invalid_argument(where + ": a position is not finite in the world");
        }
        out.positions.push_back(p);
        const glm::dvec3 n = normal_matrix * mesh.normals[i];
        const double length = glm::length(n);
        out.normals.push_back(length > 0.0 && std::isfinite(length) ? n / length : glm::dvec3(0.0));
    }
    return out;
}

void throw_on_device_error(RTCDevice device, const char* what) {
    const RTCError error = rtcGetDeviceError(device);
    if (error != RTC_ERROR_NONE) {
        throw std::runtime_error(std::string("Scene: Embree failed to ") + what + ": " +
                                 rtcGetErrorString(error));
    }
}

RTCRay make_ray(glm::dvec3 origin, glm::dvec3 direction, double t_min, double t_max) {
    RTCRay ray{};
    ray.org_x = static_cast<float>(origin.x);
    ray.org_y = static_cast<float>(origin.y);
    ray.org_z = static_cast<float>(origin.z);
    ray.dir_x = static_cast<float>(direction.x);
    ray.dir_y = static_cast<float>(direction.y);
    ray.dir_z = static_cast<float>(direction.z);
    constexpr double float_max = std::numeric_limits<float>::max();
    ray.tnear = static_cast<float>(std::min(std::max(t_min, 0.0), float_max));
    ray.tfar =
        t_max >= float_max ? std::numeric_limits<float>::infinity() : static_cast<float>(t_max);
    ray.mask = 0xffffffffu;
    ray.time = 0.0f;
    ray.flags = 0;
    return ray;
}

} // namespace

struct Scene::Impl {
    world::World world;
    std::vector<WorldMesh> meshes; // one per instance
    RTCDevice device = nullptr;
    RTCScene scene = nullptr;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() {
        if (scene != nullptr) {
            rtcReleaseScene(scene);
        }
        if (device != nullptr) {
            rtcReleaseDevice(device);
        }
    }
};

Scene::Scene(const world::World& world) : impl_(std::make_unique<Impl>()) {
    Impl& impl = *impl_;
    // Geometry ids number the instances, and Embree reserves the largest for "no hit".
    if (world.instances.size() >= RTC_INVALID_GEOMETRY_ID) {
        throw std::invalid_argument("Scene: more instances than Embree can number");
    }
    impl.world = world;
    impl.meshes.reserve(world.instances.size());
    for (std::size_t i = 0; i < world.instances.size(); ++i) {
        impl.meshes.push_back(to_world(world, i));
    }

    impl.device = rtcNewDevice(nullptr);
    if (impl.device == nullptr) {
        throw std::runtime_error("Scene: Embree could not create a device");
    }
    impl.scene = rtcNewScene(impl.device);
    // Robust: rays through a shared edge of two triangles hit one of them, so that closed rooms
    // stay closed.
    rtcSetSceneFlags(impl.scene, RTC_SCENE_FLAG_ROBUST);
    rtcSetSceneBuildQuality(impl.scene, RTC_BUILD_QUALITY_HIGH);

    for (std::size_t i = 0; i < impl.meshes.size(); ++i) {
        const WorldMesh& mesh = impl.meshes[i];
        const std::vector<std::uint32_t>& indices = world.meshes[world.instances[i].mesh].indices;
        if (indices.empty()) {
            continue; // Embree refuses a geometry without triangles; nothing to hit anyway
        }
        RTCGeometry geometry = rtcNewGeometry(impl.device, RTC_GEOMETRY_TYPE_TRIANGLE);
        auto* vertices = static_cast<float*>(
            rtcSetNewGeometryBuffer(geometry, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3,
                                    3 * sizeof(float), mesh.positions.size()));
        auto* triangles = static_cast<std::uint32_t*>(
            rtcSetNewGeometryBuffer(geometry, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
                                    3 * sizeof(std::uint32_t), indices.size() / 3));
        if (vertices == nullptr || triangles == nullptr) {
            rtcReleaseGeometry(geometry);
            throw_on_device_error(impl.device, "allocate a geometry's buffers");
            throw std::runtime_error("Scene: Embree could not allocate a geometry's buffers");
        }
        for (std::size_t v = 0; v < mesh.positions.size(); ++v) {
            vertices[3 * v + 0] = static_cast<float>(mesh.positions[v].x);
            vertices[3 * v + 1] = static_cast<float>(mesh.positions[v].y);
            vertices[3 * v + 2] = static_cast<float>(mesh.positions[v].z);
        }
        std::copy(indices.begin(), indices.end(), triangles);
        rtcCommitGeometry(geometry);
        // The geometry's id is the instance's index, so a hit names its instance.
        rtcAttachGeometryByID(impl.scene, geometry, static_cast<unsigned>(i));
        rtcReleaseGeometry(geometry);
        throw_on_device_error(impl.device, "add an instance's triangles");
    }
    rtcCommitScene(impl.scene);
    throw_on_device_error(impl.device, "build the scene");
}

Scene::~Scene() = default;
Scene::Scene(Scene&&) noexcept = default;
Scene& Scene::operator=(Scene&&) noexcept = default;

const world::World& Scene::world() const {
    return impl_->world;
}

std::optional<Hit> Scene::intersect(glm::dvec3 origin_m, glm::dvec3 direction, double t_min,
                                    double t_max) const {
    if (!(t_max >= t_min)) {
        return std::nullopt;
    }
    RTCRayHit query{};
    query.ray = make_ray(origin_m, direction, t_min, t_max);
    query.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    query.hit.primID = RTC_INVALID_GEOMETRY_ID;
    query.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
    rtcIntersect1(impl_->scene, &query);
    if (query.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
        return std::nullopt;
    }

    Hit hit;
    hit.instance = query.hit.geomID;
    hit.triangle = query.hit.primID;
    hit.distance_m = static_cast<double>(query.ray.tfar);
    hit.position_m = origin_m + hit.distance_m * direction;

    const WorldMesh& mesh = impl_->meshes[hit.instance];
    const geom::Mesh& source = impl_->world.meshes[impl_->world.instances[hit.instance].mesh];
    const std::uint32_t i0 = source.indices[3 * std::size_t{hit.triangle} + 0];
    const std::uint32_t i1 = source.indices[3 * std::size_t{hit.triangle} + 1];
    const std::uint32_t i2 = source.indices[3 * std::size_t{hit.triangle} + 2];

    const glm::dvec3 ng = glm::cross(mesh.positions[i1] - mesh.positions[i0],
                                     mesh.positions[i2] - mesh.positions[i0]);
    const double ng_length = glm::length(ng);
    if (ng_length > 0.0) {
        hit.geometric_normal = ng / ng_length;
    }

    // Embree's barycentrics: the hit is (1 - u - v) p0 + u p1 + v p2.
    const double u = static_cast<double>(query.hit.u);
    const double v = static_cast<double>(query.hit.v);
    const double w = 1.0 - u - v;
    const glm::dvec3 n = w * mesh.normals[i0] + u * mesh.normals[i1] + v * mesh.normals[i2];
    const double n_length = glm::length(n);
    hit.normal = n_length > 0.0 ? n / n_length : hit.geometric_normal;
    hit.uv = w * source.uvs[i0] + u * source.uvs[i1] + v * source.uvs[i2];
    return hit;
}

bool Scene::occluded(glm::dvec3 origin_m, glm::dvec3 direction, double t_min, double t_max) const {
    if (!(t_max >= t_min)) {
        return false;
    }
    RTCRay ray = make_ray(origin_m, direction, t_min, t_max);
    rtcOccluded1(impl_->scene, &ray);
    // Embree sets tfar to -infinity when it finds something in the way.
    return ray.tfar < 0.0f;
}

double ray_offset_m(glm::dvec3 point_m) {
    const double largest =
        std::max({std::abs(point_m.x), std::abs(point_m.y), std::abs(point_m.z)});
    return std::max(1e-5 * largest, 1e-5);
}

} // namespace cs::engine
