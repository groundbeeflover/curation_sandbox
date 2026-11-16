// SPDX-License-Identifier: Apache-2.0

// The Standard tier's exact direct light (standard_tier.hpp, issue #78, M1-22): the spot profile
// table against light::GenericSpot, the inverse-square and cosine laws in the illuminance output,
// radiance L = albedo / pi * colour * E, both sides of a surface, painting textures, and the
// tier's refusals. The skeleton project against the engine is in
// tests/integration/test_standard_vs_engine.cpp, as gpu may not depend on doc or engine.
//
// Every test that needs a device starts with make_device() (gpu_test.hpp): without a Vulkan
// device it skips, and with CS_REQUIRE_GPU=1 it fails. The harness (main.cpp) fails the running
// test on any validation warning or error, so every test here is also validation-clean. Scenes and
// tiers are declared after the device, so they go before it.

#include "gpu_test.hpp"

#include <cs/assets/texture.hpp>
#include <cs/core/colour.hpp>
#include <cs/core/image.hpp>
#include <cs/geom/mesh.hpp>
#include <cs/gpu/device.hpp>
#include <cs/gpu/pipeline.hpp>
#include <cs/gpu/scene.hpp>
#include <cs/gpu/standard_tier.hpp>
#include <cs/light/distribution.hpp>
#include <cs/light/luminaire.hpp>
#include <cs/world/ids.hpp>
#include <cs/world/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using cs::gpu::Device;
using cs::gpu::LightData;
using cs::gpu::LightTable;
using cs::gpu::Scene;
using cs::gpu::StandardImages;
using cs::gpu::StandardTier;
using cs::gpu::test::Cleanup;
using cs::gpu::test::make_device;
using glm::dvec3;

namespace geom = cs::geom;
namespace light = cs::light;
namespace world = cs::world;

namespace {

constexpr double pi = std::numbers::pi;
constexpr double degrees = pi / 180.0;

// ---- Lights ------------------------------------------------------------------------------------

world::Light spot_light(dvec3 position_m, dvec3 aim_m,
                        std::shared_ptr<const light::GenericSpot> spot, double kelvin = 3000.0) {
    return world::Light{.entity = world::Id{},
                        .luminaire = light::Luminaire(position_m, aim_m, spot),
                        .spot = spot,
                        .colour_temperature_k = kelvin};
}

// An isotropic light as the engine's tests make one: a uniform cone over the whole sphere (beam
// = field = 360 degrees), whose intensity is 4 pi I lumens over 4 pi steradians, I everywhere.
world::Light isotropic_light(dvec3 position_m, double intensity_cd, double kelvin = 3000.0) {
    return spot_light(
        position_m, position_m + dvec3(0.0, 0.0, -1.0),
        std::make_shared<const light::GenericSpot>(4.0 * pi * intensity_cd, 2.0 * pi, 2.0 * pi),
        kelvin);
}

// The light's colour as the engine uses it: linear RGB of luminance 1.
dvec3 light_colour(double kelvin) {
    const dvec3 rgb = cs::cct_to_linear_rgb(kelvin);
    return rgb / cs::luminance(rgb);
}

// Where a generic spot stops emitting (distribution.hpp): 1.5 times the half field angle, at most
// 90 degrees for a field of 180 degrees or less.
double cutoff_rad(const light::GenericSpot& spot) {
    const double half_field = 0.5 * spot.field_angle_rad();
    if (half_field <= pi / 2.0) {
        return std::min(1.5 * half_field, pi / 2.0);
    }
    return std::min({1.5 * half_field, pi, half_field + 3.0 * (half_field - pi / 2.0)});
}

// ---- Meshes and worlds -------------------------------------------------------------------------

// A rectangle in the plane z = height_m, centred on (cx, cy), facing +z: two triangles.
geom::Mesh floor_mesh(double cx, double cy, double half_x, double half_y, double height_m = 0.0) {
    geom::Mesh mesh;
    mesh.positions = {{cx - half_x, cy - half_y, height_m},
                      {cx + half_x, cy - half_y, height_m},
                      {cx + half_x, cy + half_y, height_m},
                      {cx - half_x, cy + half_y, height_m}};
    mesh.normals.assign(4, dvec3(0.0, 0.0, 1.0));
    mesh.uvs = {{0.0, 1.0}, {1.0, 1.0}, {1.0, 0.0}, {0.0, 0.0}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

world::Instance instance(std::uint32_t mesh, std::uint32_t material) {
    world::Instance result;
    result.mesh = mesh;
    result.material = material;
    return result;
}

// A large Lambert floor at z = 0, of albedo 0.8, and the given lights.
world::World floor_world(std::vector<world::Light> lights, double albedo = 0.8) {
    world::World w;
    w.meshes = {floor_mesh(0.0, 0.0, 10.0, 10.0)};
    w.materials = {world::LambertMaterial{dvec3(albedo)}};
    w.instances = {instance(0, 0)};
    w.lights = std::move(lights);
    return w;
}

// A loader for worlds without images: any call fails the test.
cs::gpu::TextureLoader no_loader() {
    return [](const world::AssetRef& ref) -> cs::assets::Texture {
        FAIL_CHECK("the loader was called for " << ref.sha256 << ", in a world without images");
        throw std::runtime_error("unexpected load");
    };
}

// ---- Cameras -----------------------------------------------------------------------------------

// An orthographic camera at height_m above (cx, cy), looking straight down, x to the right of the
// image and y up it, seeing extent_m vertically.
world::Camera looking_down(double cx, double cy, double height_m, double extent_m) {
    world::Camera camera;
    camera.projection = world::Projection::orthographic;
    camera.position_m = {cx, cy, height_m};
    camera.forward = {0.0, 0.0, -1.0};
    camera.up = {0.0, 1.0, 0.0};
    camera.vertical_extent_m = extent_m;
    return camera;
}

// The point of the plane z = 0 that pixel (x, y) of a width x height image of that camera sees,
// by the camera convention of standard_tier.hpp (the engine's).
dvec3 floor_point(const world::Camera& camera, int width, int height, int x, int y) {
    const double half_height = 0.5 * camera.vertical_extent_m;
    const double half_width = half_height * width / height;
    const double sx = (2.0 * (x + 0.5) / width - 1.0) * half_width;
    const double sy = (1.0 - 2.0 * (y + 0.5) / height) * half_height;
    return {camera.position_m.x + sx, camera.position_m.y + sy, 0.0};
}

// The pixel of that camera whose centre is nearest to a point of the floor.
std::pair<int, int> pixel_at(const world::Camera& camera, int width, int height, dvec3 point) {
    const double half_height = 0.5 * camera.vertical_extent_m;
    const double half_width = half_height * width / height;
    const double x = ((point.x - camera.position_m.x) / half_width + 1.0) * 0.5 * width - 0.5;
    const double y = (1.0 - (point.y - camera.position_m.y) / half_height) * 0.5 * height - 0.5;
    return {static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))};
}

float lux_at(const StandardImages& images, int x, int y) {
    return images.illuminance_lux.at(static_cast<std::size_t>(y) * images.radiance.width() + x);
}

// I cos / d^2 at a point of the floor (normal +z) from an isotropic light above it.
double floor_lux(dvec3 light_m, double intensity_cd, dvec3 point_m) {
    const dvec3 to_light = light_m - point_m;
    const double d = glm::length(to_light);
    return intensity_cd * (to_light.z / d) / (d * d);
}

// ---- The shader's profile, on the GPU ----------------------------------------------------------

// standard.slang's check_intensity: push constants, in C++ packing.
struct CheckPush {
    VkDeviceAddress lights = 0; // a LightTable: the lights, then the profiles
    VkDeviceAddress directions = 0;
    VkDeviceAddress intensities = 0;
    std::uint32_t light_count = 0;
    std::uint32_t light = 0;
    std::uint32_t count = 0;
    std::uint32_t padding = 0;
};
static_assert(sizeof(CheckPush) == 40);

// A host-visible buffer with a device address, mapped, freed with the object.
class MappedBuffer {
  public:
    MappedBuffer(const Device& device, VkDeviceSize size_bytes) : allocator_(device.allocator()) {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size_bytes;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo allocation_info{};
        allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
        allocation_info.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo mapped{};
        REQUIRE(vmaCreateBuffer(allocator_, &info, &allocation_info, &buffer_, &allocation_,
                                &mapped) == VK_SUCCESS);
        data_ = static_cast<std::byte*>(mapped.pMappedData);
        VkBufferDeviceAddressInfo address_info{};
        address_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        address_info.buffer = buffer_;
        address_ = vkGetBufferDeviceAddress(device.device(), &address_info);
    }
    ~MappedBuffer() { vmaDestroyBuffer(allocator_, buffer_, allocation_); }
    MappedBuffer(const MappedBuffer&) = delete;
    MappedBuffer& operator=(const MappedBuffer&) = delete;

    std::byte* data() const { return data_; }
    VkDeviceAddress address() const { return address_; }
    void flush() const {
        REQUIRE(vmaFlushAllocation(allocator_, allocation_, 0, VK_WHOLE_SIZE) == VK_SUCCESS);
    }
    void invalidate() const {
        REQUIRE(vmaInvalidateAllocation(allocator_, allocation_, 0, VK_WHOLE_SIZE) == VK_SUCCESS);
    }

  private:
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    std::byte* data_ = nullptr;
    VkDeviceAddress address_ = 0;
};

// The intensity, in candela, that the shader's intensity_cd gives for light `light` of the table
// towards each world direction, run by standard.slang's compute entry point check_intensity.
std::vector<float> shader_intensities(const Device& gpu, const LightTable& table,
                                      std::uint32_t light, std::span<const glm::vec3> directions) {
    const VkDevice device = gpu.device();
    const VkDeviceSize light_bytes = table.lights.size() * sizeof(LightData);
    const VkDeviceSize profile_bytes = table.profiles.size() * sizeof(float);
    MappedBuffer lights(gpu, light_bytes + profile_bytes);
    std::memcpy(lights.data(), table.lights.data(), light_bytes);
    std::memcpy(lights.data() + light_bytes, table.profiles.data(), profile_bytes);
    lights.flush();
    MappedBuffer inputs(gpu, directions.size_bytes());
    std::memcpy(inputs.data(), directions.data(), directions.size_bytes());
    inputs.flush();
    MappedBuffer outputs(gpu, directions.size() * sizeof(float));

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    const Cleanup cleanup([&] {
        vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyShaderModule(device, module, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);
    });

    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CheckPush)};
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &range;
    REQUIRE(vkCreatePipelineLayout(device, &layout_info, nullptr, &layout) == VK_SUCCESS);

    const std::span<const std::uint32_t> spirv = cs::gpu::shader_spirv("standard");
    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = spirv.size_bytes();
    module_info.pCode = spirv.data();
    REQUIRE(vkCreateShaderModule(device, &module_info, nullptr, &module) == VK_SUCCESS);

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = module;
    pipeline_info.stage.pName = "check_intensity";
    pipeline_info.layout = layout;
    REQUIRE(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                     &pipeline) == VK_SUCCESS);

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = gpu.queue_family();
    REQUIRE(vkCreateCommandPool(device, &pool_info, nullptr, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo command_info{};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    REQUIRE(vkAllocateCommandBuffers(device, &command_info, &commands) == VK_SUCCESS);
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    REQUIRE(vkCreateFence(device, &fence_info, nullptr, &fence) == VK_SUCCESS);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    REQUIRE(vkBeginCommandBuffer(commands, &begin) == VK_SUCCESS);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    CheckPush push;
    push.lights = lights.address();
    push.directions = inputs.address();
    push.intensities = outputs.address();
    push.light_count = static_cast<std::uint32_t>(table.lights.size());
    push.light = light;
    push.count = static_cast<std::uint32_t>(directions.size());
    vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(commands, (push.count + 63) / 64, 1, 1);
    // The shader's writes, then the host's reads after the fence.
    VkMemoryBarrier2 to_host{};
    to_host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    to_host.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    to_host.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &to_host;
    vkCmdPipelineBarrier2(commands, &dependency);
    REQUIRE(vkEndCommandBuffer(commands) == VK_SUCCESS);

    VkCommandBufferSubmitInfo command_submit{};
    command_submit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    command_submit.commandBuffer = commands;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command_submit;
    REQUIRE(vkQueueSubmit2(gpu.queue(), 1, &submit, fence) == VK_SUCCESS);
    REQUIRE(vkWaitForFences(device, 1, &fence, VK_TRUE, 60'000'000'000ull) == VK_SUCCESS);

    outputs.invalidate();
    std::vector<float> intensities(directions.size());
    std::memcpy(intensities.data(), outputs.data(), directions.size() * sizeof(float));
    return intensities;
}

// A direction theta from the luminaire's axis and phi around it, of the given length, in the
// world: the fixture's x, y and z axes are the rows of to_fixture.
dvec3 world_direction(const light::Luminaire& luminaire, double theta, double phi, double length) {
    const dvec3 local{std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi),
                      std::cos(theta)};
    const dvec3 ex = luminaire.to_fixture({1.0, 0.0, 0.0});
    const dvec3 ey = luminaire.to_fixture({0.0, 1.0, 0.0});
    const dvec3 ez = luminaire.to_fixture({0.0, 0.0, 1.0});
    const dvec3 axis_x{ex.x, ey.x, ez.x};
    const dvec3 axis_y{ex.y, ey.y, ez.y};
    const dvec3 axis_z{ex.z, ey.z, ez.z};
    return length * (local.x * axis_x + local.y * axis_y + local.z * axis_z);
}

struct ProfileDirection {
    double theta_rad;
    bool beyond; // beyond the cut-off or behind the fixture: exactly 0
};

// The issue's 100 directions for a spot: the beam axis, three within a milliradian of it (where
// a profile with p < 1 comes to a point), half the beam angle, twelve between half the field
// angle and the cut-off, one 1e-4 rad beyond the cut-off, three behind the fixture, and 79 evenly
// over [0, cut-off].
std::vector<ProfileDirection> profile_directions(const light::GenericSpot& spot) {
    const double half_field = 0.5 * spot.field_angle_rad();
    const double cutoff = cutoff_rad(spot);
    std::vector<ProfileDirection> directions{{0.0, false},
                                             {1e-5, false},
                                             {1e-4, false},
                                             {1e-3, false},
                                             {0.5 * spot.beam_angle_rad(), false}};
    for (int k = 0; k < 12; ++k) {
        directions.push_back({half_field + (cutoff - half_field) * (k + 0.5) / 12.0, false});
    }
    directions.push_back({cutoff + 1e-4, true});
    for (const double theta : {100.0 * degrees, 135.0 * degrees, 180.0 * degrees}) {
        directions.push_back({theta, true});
    }
    for (int k = 0; k < 79; ++k) {
        directions.push_back({cutoff * (k + 0.5) / 79.0, false});
    }
    return directions;
}

} // namespace

// ---- The profile table -------------------------------------------------------------------------

TEST_CASE("standard: the shader's spot profile matches light::GenericSpot within 1e-4 of the "
          "peak at 100 directions, and is exactly 0 beyond the cut-off",
          "[gpu][standard][accuracy]") {
    const std::unique_ptr<Device> gpu = make_device();

    struct Case {
        const char* what;
        double beam_deg;
        double field_deg;
    };
    // The skeleton's spot, a wide one whose profile comes to a point on the axis (p = 0.87), and
    // one whose taper is half a degree wide (89.5 to 90 degrees).
    for (const Case& c :
         {Case{"15/25", 15.0, 25.0}, Case{"10/40", 10.0, 40.0}, Case{"90/179", 90.0, 179.0}}) {
        INFO("spot " << c.what);
        const auto spot = std::make_shared<const light::GenericSpot>(800.0, c.beam_deg * degrees,
                                                                     c.field_deg * degrees);
        // Aimed obliquely, so that every axis of the fixture matters.
        const world::Light light = spot_light({1.0, 2.0, 3.0}, {1.4, 1.5, 0.2}, spot);
        const LightTable table = cs::gpu::make_light_table(std::span(&light, 1));

        const std::vector<ProfileDirection> cases = profile_directions(*spot);
        REQUIRE(cases.size() == 100);
        std::vector<glm::vec3> directions;
        std::vector<double> expected;
        for (std::size_t k = 0; k < cases.size(); ++k) {
            // Assorted azimuths (the golden angle apart) and lengths, as the shader must
            // normalise.
            const double phi = 2.39996 * static_cast<double>(k);
            const double length = 0.5 + 0.7 * static_cast<double>(k % 5);
            const dvec3 d = world_direction(light.luminaire, cases[k].theta_rad, phi, length);
            directions.emplace_back(d);
            expected.push_back(light.luminaire.intensity_cd(d));
        }
        const std::vector<float> got = shader_intensities(*gpu, table, 0, directions);

        // 1e-4 of the peak, the issue's tolerance: the shader works in float, so a tolerance in
        // candela must scale with the thousands of candela of a spot. The table's interpolation
        // and the angle's rounding in float stay well inside it.
        const double peak = spot->peak_intensity_cd();
        int between_field_and_cutoff = 0;
        for (std::size_t k = 0; k < cases.size(); ++k) {
            INFO("direction " << k << ": theta " << cases[k].theta_rad / degrees
                              << " degrees; expected " << expected[k] << " cd, got " << got[k]);
            if (cases[k].beyond) {
                CHECK(expected[k] == 0.0);
                CHECK(got[k] == 0.0f);
            } else {
                CHECK(std::abs(got[k] - expected[k]) <= 1e-4 * peak);
            }
            if (cases[k].theta_rad > 0.5 * spot->field_angle_rad() &&
                cases[k].theta_rad < cutoff_rad(*spot)) {
                ++between_field_and_cutoff;
            }
        }
        CHECK(between_field_and_cutoff >= 10);
    }
}

TEST_CASE("standard: make_light_table gives each light its position, axes, peak, colour of "
          "luminance 1 and a profile table whose first entry is 1",
          "[gpu][standard]") {
    const auto spot =
        std::make_shared<const light::GenericSpot>(800.0, 15.0 * degrees, 25.0 * degrees);
    const std::vector<world::Light> lights{
        spot_light({3.465, 1.097, 3.5}, {3.0, 0.04, 1.5}, spot, 3000.0),
        isotropic_light({0.0, 0.0, 2.0}, 1000.0, 6500.0)};
    const LightTable table = cs::gpu::make_light_table(lights);

    REQUIRE(table.lights.size() == 2);
    REQUIRE(table.profiles.size() == 2 * cs::gpu::profile_entries);
    for (std::size_t k = 0; k < lights.size(); ++k) {
        INFO("light " << k);
        const LightData& data = table.lights[k];
        const light::Luminaire& luminaire = lights[k].luminaire;
        CHECK(data.profile == k * cs::gpu::profile_entries);
        CHECK(table.profiles[data.profile] == 1.0f);
        CHECK(data.position_m == glm::vec3(luminaire.position_m()));
        CHECK(data.peak_cd == static_cast<float>(lights[k].spot->peak_intensity_cd()));
        // The axes, unit and right-handed, z along the beam.
        CHECK_THAT(glm::distance(glm::dvec3(data.axis_z), luminaire.axis()), WithinAbs(0.0, 1e-6));
        // The fixture's x axis: to_fixture's first row.
        const dvec3 axis_x{luminaire.to_fixture({1.0, 0.0, 0.0}).x,
                           luminaire.to_fixture({0.0, 1.0, 0.0}).x,
                           luminaire.to_fixture({0.0, 0.0, 1.0}).x};
        CHECK_THAT(glm::distance(glm::dvec3(data.axis_x), axis_x), WithinAbs(0.0, 1e-6));
        CHECK_THAT(glm::length(glm::cross(glm::dvec3(data.axis_x), glm::dvec3(data.axis_y)) -
                               glm::dvec3(data.axis_z)),
                   WithinAbs(0.0, 1e-6));
        // As the engine colours its lamps; float, so within 1e-6.
        const dvec3 colour = light_colour(lights[k].colour_temperature_k);
        CHECK_THAT(data.colour.r, WithinAbs(colour.r, 1e-6));
        CHECK_THAT(data.colour.g, WithinAbs(colour.g, 1e-6));
        CHECK_THAT(data.colour.b, WithinAbs(colour.b, 1e-6));
        CHECK_THAT(cs::luminance(glm::dvec3(data.colour)), WithinAbs(1.0, 1e-6));
        // Every entry a fraction of the peak.
        for (std::uint32_t i = 0; i < cs::gpu::profile_entries; ++i) {
            const float entry = table.profiles[data.profile + i];
            if (!(entry >= 0.0f && entry <= 1.0f)) {
                FAIL_CHECK("entry " << i << " is " << entry);
            }
        }
    }

    // The tapered spot: the core ends at half the field angle and the table just short of the
    // cut-off, as sin(theta / 2).
    CHECK_THAT(table.lights[0].core_end, WithinAbs(std::sin(12.5 * degrees / 2.0), 1e-7));
    CHECK_THAT(table.lights[0].lit_end, WithinAbs(std::sin(cutoff_rad(*spot) / 2.0), 1e-6));
    CHECK(table.lights[0].lit_end < static_cast<float>(std::sin(cutoff_rad(*spot) / 2.0)) + 1e-7f);
    // The isotropic light lights the whole sphere: w reaches sin(90 degrees) = 1, and every entry
    // is 1.
    CHECK(table.lights[1].lit_end == 1.0f);
    for (std::uint32_t i = 0; i < cs::gpu::profile_entries; ++i) {
        if (table.profiles[table.lights[1].profile + i] != 1.0f) {
            FAIL_CHECK("isotropic entry " << i << " is "
                                          << table.profiles[table.lights[1].profile + i]);
        }
    }
}

TEST_CASE("standard: a uniform cone keeps its hard edge: its table ends at half its beam angle",
          "[gpu][standard]") {
    const auto cone =
        std::make_shared<const light::GenericSpot>(500.0, 30.0 * degrees, 30.0 * degrees);
    const world::Light light = spot_light({0.0, 0.0, 2.0}, {0.0, 0.0, 0.0}, cone);
    const LightTable table = cs::gpu::make_light_table(std::span(&light, 1));
    REQUIRE(table.lights.size() == 1);
    CHECK_THAT(table.lights[0].lit_end, WithinAbs(std::sin(15.0 * degrees / 2.0), 1e-7));
    CHECK(table.lights[0].core_end == table.lights[0].lit_end);
    for (std::uint32_t i = 0; i < cs::gpu::profile_core_entries; ++i) {
        if (table.profiles[i] != 1.0f) {
            FAIL_CHECK("core entry " << i << " is " << table.profiles[i]);
        }
    }
}

TEST_CASE("standard: make_light_table refuses a light without a generic spot, and more than "
          "max_standard_lights lights",
          "[gpu][standard]") {
    world::Light no_spot = isotropic_light({0.0, 0.0, 2.0}, 100.0);
    no_spot.spot = nullptr;
    CHECK_THROWS_AS(cs::gpu::make_light_table(std::span(&no_spot, 1)), std::invalid_argument);

    const std::vector<world::Light> too_many(cs::gpu::max_standard_lights + 1,
                                             isotropic_light({0.0, 0.0, 2.0}, 100.0));
    CHECK_THROWS_AS(cs::gpu::make_light_table(too_many), std::invalid_argument);
    const std::vector<world::Light> enough(cs::gpu::max_standard_lights,
                                           isotropic_light({0.0, 0.0, 2.0}, 100.0));
    CHECK(cs::gpu::make_light_table(enough).lights.size() == cs::gpu::max_standard_lights);
}

// ---- Exact direct light ------------------------------------------------------------------------

TEST_CASE("standard: the floor under a 1000 cd light 2 m above reads 250 lux, every pixel reads "
          "I cos / d^2 and its radiance is albedo / pi times colour times E",
          "[gpu][standard][accuracy]") {
    const std::unique_ptr<Device> gpu = make_device();
    const dvec3 light_m{0.0, 0.0, 2.0};
    const world::World w = floor_world({isotropic_light(light_m, 1000.0)});
    const Scene scene(*gpu, w, no_loader());
    // 65 x 65 pixels over 4 m: the centre pixel's centre sees the origin, under the light.
    constexpr int size = 65;
    const StandardTier tier(*gpu, scene, w.lights, size, size);
    const world::Camera camera = looking_down(0.0, 0.0, 1.0, 4.0);
    const StandardImages images = tier.render(camera);
    REQUIRE(images.radiance.width() == size);
    REQUIRE(images.radiance.height() == size);
    REQUIRE(images.illuminance_lux.size() == std::size_t{size} * size);

    // The issue's criterion: 1000 / 2^2 = 250 lux within 1%.
    CHECK_THAT(lux_at(images, size / 2, size / 2), WithinRel(250.0, 0.01));

    // Every pixel against the analytic value at the exact point it sees. 1e-4: the GPU computes
    // in float (about 6e-8 relative per operation, a few dozen of them) and the isotropic light's
    // table is all ones, so anything beyond a few 1e-6 is a wrong distance, cosine or point.
    const dvec3 colour = light_colour(3000.0);
    double worst_lux = 0.0;
    double worst_radiance = 0.0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const dvec3 point = floor_point(camera, size, size, x, y);
            const double expected = floor_lux(light_m, 1000.0, point);
            worst_lux = std::max(worst_lux, std::abs(lux_at(images, x, y) / expected - 1.0));
            const cs::Rgb& radiance = images.radiance.at(x, y);
            const dvec3 expected_radiance = 0.8 / pi * expected * colour;
            worst_radiance =
                std::max({worst_radiance, std::abs(radiance.r / expected_radiance.r - 1.0),
                          std::abs(radiance.g / expected_radiance.g - 1.0),
                          std::abs(radiance.b / expected_radiance.b - 1.0)});
        }
    }
    CHECK(worst_lux <= 1e-4);
    CHECK(worst_radiance <= 1e-4);
}

TEST_CASE("standard: a perspective camera reads the same lux at the same point as an orthographic "
          "one",
          "[gpu][standard][accuracy]") {
    const std::unique_ptr<Device> gpu = make_device();
    const world::World w = floor_world({isotropic_light({0.0, 0.0, 2.0}, 1000.0)});
    const Scene scene(*gpu, w, no_loader());
    constexpr int size = 33;
    const StandardTier tier(*gpu, scene, w.lights, size, size);
    // Looking straight down from 1.5 m with a 60 degree field: the centre pixel sees the origin.
    world::Camera camera;
    camera.position_m = {0.0, 0.0, 1.5};
    camera.forward = {0.0, 0.0, -1.0};
    camera.up = {0.0, 1.0, 0.0};
    camera.vertical_fov_rad = 60.0 * degrees;
    const StandardImages images = tier.render(camera);
    // 1e-4 as above.
    CHECK_THAT(lux_at(images, size / 2, size / 2), WithinRel(250.0, 1e-4));
}

TEST_CASE("standard: two lights add; a light behind the surface adds nothing; the back of a "
          "surface is lit like its front",
          "[gpu][standard][accuracy]") {
    const std::unique_ptr<Device> gpu = make_device();
    constexpr int size = 33;
    const world::Camera from_above = looking_down(0.0, 0.0, 1.0, 2.0);
    world::Camera from_below = from_above;
    from_below.position_m = {0.0, 0.0, -1.0};
    from_below.forward = {0.0, 0.0, 1.0};
    // 1e-4 as in the inverse-square test.
    constexpr double tolerance = 1e-4;

    SECTION("two lights") {
        const world::World w = floor_world(
            {isotropic_light({0.0, 0.0, 2.0}, 1000.0), isotropic_light({1.0, 0.0, 2.0}, 500.0)});
        const Scene scene(*gpu, w, no_loader());
        const StandardTier tier(*gpu, scene, w.lights, size, size);
        const StandardImages images = tier.render(from_above);
        // 250 + 500 * 2 / sqrt(5)^3 at the origin.
        const double expected =
            floor_lux({0.0, 0.0, 2.0}, 1000.0, {}) + floor_lux({1.0, 0.0, 2.0}, 500.0, {});
        CHECK_THAT(lux_at(images, size / 2, size / 2), WithinRel(expected, tolerance));
    }
    SECTION("a light below the floor, seen from above, adds nothing") {
        const world::World w = floor_world({isotropic_light({0.0, 0.0, -2.0}, 1000.0)});
        const Scene scene(*gpu, w, no_loader());
        const StandardTier tier(*gpu, scene, w.lights, size, size);
        const StandardImages images = tier.render(from_above);
        for (const float lux : images.illuminance_lux) {
            if (lux != 0.0f) {
                FAIL_CHECK("a pixel reads " << lux << " lux from a light behind the surface");
                break;
            }
        }
        CHECK(images.radiance.at(size / 2, size / 2).r == 0.0f);
    }
    SECTION("the floor's underside, seen from below and lit from below, reads like its top") {
        const world::World w = floor_world({isotropic_light({0.0, 0.0, -2.0}, 1000.0)});
        const Scene scene(*gpu, w, no_loader());
        const StandardTier tier(*gpu, scene, w.lights, size, size);
        const StandardImages images = tier.render(from_below);
        CHECK_THAT(lux_at(images, size / 2, size / 2), WithinRel(250.0, tolerance));
        CHECK(images.radiance.at(size / 2, size / 2).g > 0.0f);
    }
}

TEST_CASE("standard: a painting reflects its texture's texel, one without an image its base "
          "colour, and albedos are clamped to [0, 1]",
          "[gpu][standard][accuracy]") {
    const std::unique_ptr<Device> gpu = make_device();

    // Three 1 m squares on the floor, under an isotropic light 3 m above the middle one.
    world::World w;
    w.meshes = {floor_mesh(-1.5, 0.0, 0.5, 0.5), floor_mesh(0.0, 0.0, 0.5, 0.5),
                floor_mesh(1.5, 0.0, 0.5, 0.5)};
    const world::AssetRef asset{std::string(64, 'a'), "png"};
    world::PaintingSurface textured;
    textured.parameters.base.color = dvec3(0.9, 0.1, 0.1); // not what is shown
    textured.image = asset;
    world::PaintingSurface untextured;
    untextured.parameters.base.color = dvec3(0.1, 0.3, 0.6);
    w.materials = {textured, untextured, world::LambertMaterial{dvec3(1.5, 0.5, -0.25)}};
    w.instances = {instance(0, 0), instance(1, 1), instance(2, 2)};
    const dvec3 light_m{0.0, 0.0, 3.0};
    w.lights = {isotropic_light(light_m, 1000.0)};

    // One colour everywhere, so that filtering cannot change it.
    const cs::Rgb texel{0.2f, 0.5f, 0.7f};
    cs::Image image(8, 8);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            image.at(x, y) = texel;
        }
    }
    const std::map<std::string, cs::assets::Texture> textures{
        {asset.sha256, cs::assets::Texture{cs::assets::build_mip_chain(image)}}};
    const Scene scene(*gpu, w,
                      [&textures](const world::AssetRef& ref) { return textures.at(ref.sha256); });
    REQUIRE(scene.problems().empty());

    constexpr int width = 91;
    constexpr int height = 31;
    // 1.5 m high and 4.4 m wide: all three squares are in view.
    const world::Camera camera = looking_down(0.0, 0.0, 1.0, 1.5);
    const StandardTier tier(*gpu, scene, w.lights, width, height);
    const StandardImages images = tier.render(camera);

    // Textures are half floats (scene.hpp): a texel in [0, 1] is off by up to 2.4e-4 of itself,
    // so 1e-3; the base colours and clamps are floats, held to 1e-4 as above.
    struct Square {
        const char* what;
        dvec3 centre;
        dvec3 albedo;
        double tolerance;
    };
    const dvec3 colour = light_colour(3000.0);
    for (const Square& s :
         {Square{"textured painting", {-1.5, 0.0, 0.0}, {0.2, 0.5, 0.7}, 1e-3},
          Square{"painting without an image", {0.0, 0.0, 0.0}, {0.1, 0.3, 0.6}, 1e-4},
          Square{"Lambert, clamped", {1.5, 0.0, 0.0}, {1.0, 0.5, 0.0}, 1e-4}}) {
        INFO(s.what);
        const auto [x, y] = pixel_at(camera, width, height, s.centre);
        const dvec3 point = floor_point(camera, width, height, x, y);
        const double lux = floor_lux(light_m, 1000.0, point);
        CHECK_THAT(lux_at(images, x, y), WithinRel(lux, 1e-4));
        const cs::Rgb& radiance = images.radiance.at(x, y);
        const dvec3 expected = s.albedo / pi * lux * colour;
        CHECK_THAT(radiance.r, WithinRel(expected.r, s.tolerance) || WithinAbs(expected.r, 1e-6));
        CHECK_THAT(radiance.g, WithinRel(expected.g, s.tolerance) || WithinAbs(expected.g, 1e-6));
        CHECK_THAT(radiance.b, WithinRel(expected.b, s.tolerance) || WithinAbs(expected.b, 1e-6));
    }
}

// ---- Empty views, sizes, cameras ---------------------------------------------------------------

TEST_CASE("standard: pixels that see nothing are 0 in all four channels, and an empty world "
          "renders black",
          "[gpu][standard]") {
    const std::unique_ptr<Device> gpu = make_device();
    constexpr int size = 17;

    SECTION("a small square in a larger view") {
        world::World w;
        w.meshes = {floor_mesh(0.0, 0.0, 0.25, 0.25)};
        w.materials = {world::LambertMaterial{dvec3(0.8)}};
        w.instances = {instance(0, 0)};
        w.lights = {isotropic_light({0.0, 0.0, 2.0}, 1000.0)};
        const Scene scene(*gpu, w, no_loader());
        const StandardTier tier(*gpu, scene, w.lights, size, size);
        const StandardImages images = tier.render(looking_down(0.0, 0.0, 1.0, 2.0));
        CHECK(lux_at(images, size / 2, size / 2) > 0.0f);
        for (const auto [x, y] : {std::pair{0, 0}, std::pair{size - 1, 0}, std::pair{0, size - 1},
                                  std::pair{size - 1, size - 1}}) {
            const cs::Rgb& p = images.radiance.at(x, y);
            CHECK(p.r == 0.0f);
            CHECK(p.g == 0.0f);
            CHECK(p.b == 0.0f);
            CHECK(lux_at(images, x, y) == 0.0f);
        }
    }
    SECTION("a world without meshes, with a light") {
        world::World w;
        w.lights = {isotropic_light({0.0, 0.0, 2.0}, 1000.0)};
        const Scene scene(*gpu, w, no_loader());
        const StandardTier tier(*gpu, scene, w.lights, size, size);
        const StandardImages images = tier.render(looking_down(0.0, 0.0, 1.0, 2.0));
        CHECK(std::all_of(images.illuminance_lux.begin(), images.illuminance_lux.end(),
                          [](float lux) { return lux == 0.0f; }));
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const cs::Rgb& p = images.radiance.at(x, y);
                if (p.r != 0.0f || p.g != 0.0f || p.b != 0.0f) {
                    FAIL_CHECK("pixel (" << x << ", " << y << ") is not black");
                }
            }
        }
    }
    SECTION("a world with meshes and no lights") {
        const world::World w = floor_world({});
        const Scene scene(*gpu, w, no_loader());
        const StandardTier tier(*gpu, scene, w.lights, size, size);
        CHECK(tier.light_count() == 0);
        const StandardImages images = tier.render(looking_down(0.0, 0.0, 1.0, 2.0));
        CHECK(std::all_of(images.illuminance_lux.begin(), images.illuminance_lux.end(),
                          [](float lux) { return lux == 0.0f; }));
    }
}

TEST_CASE("standard: the tier refuses a zero size and an invalid camera", "[gpu][standard]") {
    const std::unique_ptr<Device> gpu = make_device();
    const world::World w = floor_world({isotropic_light({0.0, 0.0, 2.0}, 1000.0)});
    const Scene scene(*gpu, w, no_loader());
    CHECK_THROWS_AS(StandardTier(*gpu, scene, w.lights, 0, 16), std::invalid_argument);
    CHECK_THROWS_AS(StandardTier(*gpu, scene, w.lights, 16, 0), std::invalid_argument);

    const StandardTier tier(*gpu, scene, w.lights, 16, 9);
    CHECK(tier.width() == 16);
    CHECK(tier.height() == 9);
    const world::Camera good = looking_down(0.0, 0.0, 1.0, 2.0);
    CHECK_NOTHROW((void)tier.render(good));

    world::Camera parallel = good;
    parallel.up = {0.0, 0.0, 1.0}; // along forward
    CHECK_THROWS_AS((void)tier.render(parallel), std::invalid_argument);
    world::Camera not_finite = good;
    not_finite.position_m.x = std::numeric_limits<double>::quiet_NaN();
    CHECK_THROWS_AS((void)tier.render(not_finite), std::invalid_argument);
    world::Camera zero_forward = good;
    zero_forward.forward = {0.0, 0.0, 0.0};
    CHECK_THROWS_AS((void)tier.render(zero_forward), std::invalid_argument);
    world::Camera no_extent = good;
    no_extent.vertical_extent_m = 0.0;
    CHECK_THROWS_AS((void)tier.render(no_extent), std::invalid_argument);
    world::Camera perspective = good;
    perspective.projection = world::Projection::perspective;
    perspective.vertical_fov_rad = 0.0;
    CHECK_THROWS_AS((void)tier.render(perspective), std::invalid_argument);
    perspective.vertical_fov_rad = pi;
    CHECK_THROWS_AS((void)tier.render(perspective), std::invalid_argument);

    // The tier still renders after refusing.
    CHECK_THAT(tier.render(good).illuminance_lux.at(4 * 16 + 8), WithinRel(250.0, 0.05));
}

TEST_CASE("standard: record() into the caller's command buffer, twice in a row, leaves the HDR "
          "image ready for shader reads, validation-clean",
          "[gpu][standard]") {
    const std::unique_ptr<Device> gpu = make_device();
    const world::World w = floor_world({isotropic_light({0.0, 0.0, 2.0}, 1000.0)});
    const Scene scene(*gpu, w, no_loader());
    const StandardTier tier(*gpu, scene, w.lights, 32, 18);
    const world::Camera camera = looking_down(0.0, 0.0, 1.0, 2.0);

    const VkDevice device = gpu->device();
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    const Cleanup cleanup([&] {
        vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
    });
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = gpu->queue_family();
    REQUIRE(vkCreateCommandPool(device, &pool_info, nullptr, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo command_info{};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    REQUIRE(vkAllocateCommandBuffers(device, &command_info, &commands) == VK_SUCCESS);
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    REQUIRE(vkCreateFence(device, &fence_info, nullptr, &fence) == VK_SUCCESS);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    REQUIRE(vkBeginCommandBuffer(commands, &begin) == VK_SUCCESS);
    tier.record(commands, camera);
    tier.record(commands, camera);
    // The image is in SHADER_READ_ONLY_OPTIMAL: a transition from it to GENERAL is valid only if
    // it is, which the validation layer checks at submission.
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = tier.hdr_image();
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commands, &dependency);
    REQUIRE(vkEndCommandBuffer(commands) == VK_SUCCESS);

    VkCommandBufferSubmitInfo command_submit{};
    command_submit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    command_submit.commandBuffer = commands;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command_submit;
    REQUIRE(vkQueueSubmit2(gpu->queue(), 1, &submit, fence) == VK_SUCCESS);
    REQUIRE(vkWaitForFences(device, 1, &fence, VK_TRUE, 60'000'000'000ull) == VK_SUCCESS);

    // And render() still works after the image was left in GENERAL: every frame starts from
    // UNDEFINED.
    const StandardImages images = tier.render(camera);
    CHECK_THAT(lux_at(images, 16, 9), WithinRel(250.0, 0.05));
}
