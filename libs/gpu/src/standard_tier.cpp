// SPDX-License-Identifier: Apache-2.0

#include "cs/gpu/standard_tier.hpp"

#include "cs/gpu/pipeline.hpp"
#include "upload.hpp"

#include <cs/core/colour.hpp>
#include <cs/light/distribution.hpp>
#include <cs/light/luminaire.hpp>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace cs::gpu {

namespace {

using detail::check;

constexpr double pi = std::numbers::pi;

// ---- The profile table -------------------------------------------------------------------------

// The spot's intensity theta radians from its axis, as a fraction of its peak.
double relative_intensity(const light::GenericSpot& spot, double theta_rad) {
    return spot.intensity_cd({std::sin(theta_rad), 0.0, std::cos(theta_rad)}) /
           spot.peak_intensity_cd();
}

// The largest angle at which the spot emits anything, to the last bit: pi if it lights the
// whole sphere, else found by bisection, as the intensity only falls with the angle.
double lit_end_rad(const light::GenericSpot& spot) {
    if (relative_intensity(spot, pi) > 0.0)
        return pi;
    double lit = 0.0; // the axis is always lit
    double dark = pi;
    for (;;) {
        const double middle = lit + 0.5 * (dark - lit);
        if (!(middle > lit && middle < dark))
            return lit;
        (relative_intensity(spot, middle) > 0.0 ? lit : dark) = middle;
    }
}

// Appends the spot's table to `profiles` (standard_tier.hpp says how it is laid out) and sets
// the light's core_end and lit_end.
void append_profile(const light::GenericSpot& spot, LightData& light,
                    std::vector<float>& profiles) {
    const double lit_end = lit_end_rad(spot);
    const double core_end = std::min(0.5 * spot.field_angle_rad(), lit_end);
    const double w_core = std::sin(0.5 * core_end);
    const double w_lit = std::sin(0.5 * lit_end);
    light.core_end = static_cast<float>(w_core);
    light.lit_end = static_cast<float>(w_lit);
    // The angle of a table entry at w = sin(theta / 2), never past the last lit angle, which
    // the round trip through sin and asin could step over at a hard edge.
    const auto entry = [&](double w) {
        const double theta = std::min(2.0 * std::asin(std::min(w, 1.0)), lit_end);
        return static_cast<float>(relative_intensity(spot, theta));
    };
    for (std::uint32_t i = 0; i < profile_core_entries; ++i) {
        const double s = static_cast<double>(i) / (profile_core_entries - 1);
        profiles.push_back(entry(w_core * s * s));
    }
    for (std::uint32_t i = 0; i < profile_taper_entries; ++i) {
        const double t = static_cast<double>(i) / (profile_taper_entries - 1);
        profiles.push_back(entry(w_core + (w_lit - w_core) * t));
    }
}

// ---- The camera --------------------------------------------------------------------------------

// A perspective camera sees from this far in front of it (to infinity); an orthographic one
// from its plane to this far ahead (standard_tier.hpp).
constexpr double near_m = 0.01;
constexpr double orthographic_far_m = 10'000.0;

bool finite(glm::dvec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// The forward pass's push constants (standard.slang's ForwardPush): 120 bytes.
struct ForwardPush {
    std::array<glm::vec4, 4> view_projection; // columns
    glm::vec4 eye{0.0f};
    VkDeviceAddress vertices = 0;
    VkDeviceAddress instances = 0;
    VkDeviceAddress materials = 0;
    VkDeviceAddress lights = 0;
    std::uint32_t light_count = 0;
    std::uint32_t padding = 0;
};
static_assert(sizeof(ForwardPush) == 120);

// The view of a camera as the engine's path tracer makes it (engine/path_tracer.cpp,
// CameraRays): the same checks, the same axes, so that a pixel sees along the same ray. Depth is
// reversed (1 near, 0 far) for float precision, and a perspective camera's far plane is at
// infinity: z_clip = near, w_clip = the distance along forward.
void set_view(ForwardPush& push, const world::Camera& camera, std::uint32_t width,
              std::uint32_t height) {
    if (!finite(camera.position_m))
        throw std::invalid_argument("StandardTier: the camera's position is not finite");
    const double forward_length = glm::length(camera.forward);
    const double up_length = glm::length(camera.up);
    if (!(forward_length > 0.0) || !(up_length > 0.0) || !std::isfinite(forward_length) ||
        !std::isfinite(up_length))
        throw std::invalid_argument(
            "StandardTier: the camera's forward or up is not finite or is zero");
    const glm::dvec3 forward = camera.forward / forward_length;
    const glm::dvec3 right_unnormalised = glm::cross(forward, camera.up / up_length);
    const double right_length = glm::length(right_unnormalised);
    if (!(right_length > 1e-9))
        throw std::invalid_argument("StandardTier: the camera's forward and up are parallel");
    const glm::dvec3 right = right_unnormalised / right_length;
    const glm::dvec3 up = glm::cross(right, forward);

    const bool perspective = camera.projection == world::Projection::perspective;
    double half_height = 0.0;
    if (perspective) {
        if (!(camera.vertical_fov_rad > 0.0 && camera.vertical_fov_rad < pi))
            throw std::invalid_argument(
                "StandardTier: the camera's vertical field of view is not in (0, pi)");
        half_height = std::tan(0.5 * camera.vertical_fov_rad);
    } else {
        if (!(camera.vertical_extent_m > 0.0) || !std::isfinite(camera.vertical_extent_m))
            throw std::invalid_argument(
                "StandardTier: the camera's vertical extent is not finite and positive");
        half_height = 0.5 * camera.vertical_extent_m;
    }
    const double half_width = half_height * width / height;

    // Row by row: clip x and y are the image plane's coordinates over its half extents, y
    // pointing down as Vulkan's does.
    const glm::dvec3 p = camera.position_m;
    const glm::dvec4 x_row(right / half_width, -glm::dot(right, p) / half_width);
    const glm::dvec4 y_row(-up / half_height, glm::dot(up, p) / half_height);
    glm::dvec4 z_row(0.0, 0.0, 0.0, near_m);
    glm::dvec4 w_row(forward, -glm::dot(forward, p));
    if (!perspective) {
        z_row = glm::dvec4(-forward / orthographic_far_m,
                           1.0 + glm::dot(forward, p) / orthographic_far_m);
        w_row = glm::dvec4(0.0, 0.0, 0.0, 1.0);
    }
    const glm::dmat4 view_projection = glm::transpose(glm::dmat4(x_row, y_row, z_row, w_row));
    for (int c = 0; c < 4; ++c)
        push.view_projection[c] = glm::vec4(view_projection[c]);
    push.eye = perspective ? glm::vec4(glm::vec3(p), 1.0f) : glm::vec4(glm::vec3(-forward), 0.0f);
}

// ---- Images ------------------------------------------------------------------------------------

constexpr VkImageUsageFlags hdr_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
constexpr VkImageUsageFlags depth_usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
constexpr VkDeviceSize hdr_texel_bytes = 16;
constexpr VkDeviceSize depth_texel_bytes = 4; // D32_SFLOAT and X8_D24_UNORM_PACK32
constexpr VkFormat white_format = VK_FORMAT_R8G8B8A8_UNORM;

constexpr VkImageSubresourceRange depth_range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

bool supports(VkPhysicalDevice physical_device, VkFormat format, VkFormatFeatureFlags features) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physical_device, format, &properties);
    return (properties.optimalTilingFeatures & features) == features;
}

// Whether the device can make a 2D image this large in this format and use (maxExtent and
// maxResourceSize, which can be smaller than maxImageDimension2D allows).
bool image_fits(VkPhysicalDevice physical_device, VkFormat format, VkImageUsageFlags usage,
                std::uint32_t width, std::uint32_t height, VkDeviceSize texel_bytes) {
    VkImageFormatProperties properties{};
    if (vkGetPhysicalDeviceImageFormatProperties(physical_device, format, VK_IMAGE_TYPE_2D,
                                                 VK_IMAGE_TILING_OPTIMAL, usage, 0,
                                                 &properties) != VK_SUCCESS)
        return false;
    return width <= properties.maxExtent.width && height <= properties.maxExtent.height &&
           VkDeviceSize{width} * height <= properties.maxResourceSize / texel_bytes;
}

void create_image(VmaAllocator allocator, VkDevice device, VkFormat format, VkImageUsageFlags usage,
                  std::uint32_t width, std::uint32_t height, VkImageAspectFlags aspect,
                  VkImage& image, VmaAllocation& allocation, VkImageView& view) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    check(vmaCreateImage(allocator, &info, &allocation_info, &image, &allocation, nullptr),
          "allocating an image", "StandardTier");

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {aspect, 0, 1, 0, 1};
    check(vkCreateImageView(device, &view_info, nullptr, &view), "creating an image view",
          "StandardTier");
}

void destroy_image(VmaAllocator allocator, VkDevice device, VkImage image, VmaAllocation allocation,
                   VkImageView view) noexcept {
    vkDestroyImageView(device, view, nullptr);
    if (image != VK_NULL_HANDLE)
        vmaDestroyImage(allocator, image, allocation);
}

} // namespace

// ---- Lights ------------------------------------------------------------------------------------

LightTable make_light_table(std::span<const world::Light> lights) {
    if (lights.size() > max_standard_lights)
        throw std::invalid_argument("make_light_table: " + std::to_string(lights.size()) +
                                    " lights; the Standard tier takes at most " +
                                    std::to_string(max_standard_lights));
    LightTable table;
    table.lights.reserve(lights.size());
    table.profiles.reserve(lights.size() * profile_entries);
    for (std::size_t k = 0; k < lights.size(); ++k) {
        const world::Light& light = lights[k];
        if (!light.spot)
            throw std::invalid_argument("make_light_table: light " + std::to_string(k) +
                                        " has no generic spot");
        const light::Luminaire& luminaire = light.luminaire;
        // to_fixture's rows are the fixture's axes in the world.
        const glm::dvec3 ex = luminaire.to_fixture({1.0, 0.0, 0.0});
        const glm::dvec3 ey = luminaire.to_fixture({0.0, 1.0, 0.0});
        const glm::dvec3 ez = luminaire.to_fixture({0.0, 0.0, 1.0});
        LightData data;
        data.position_m = glm::vec3(luminaire.position_m());
        data.peak_cd = static_cast<float>(light.spot->peak_intensity_cd());
        data.axis_x = glm::vec3(ex.x, ey.x, ez.x);
        data.axis_y = glm::vec3(ex.y, ey.y, ez.y);
        data.axis_z = glm::vec3(ex.z, ey.z, ez.z);
        data.profile = static_cast<std::uint32_t>(k * profile_entries);
        // As the engine colours its lamps (engine/path_tracer.cpp).
        const glm::dvec3 rgb = cct_to_linear_rgb(light.colour_temperature_k);
        const double y = luminance(rgb);
        data.colour = glm::vec3(y > 0.0 ? rgb / y : glm::dvec3(1.0));
        append_profile(*light.spot, data, table.profiles);
        table.lights.push_back(data);
    }
    return table;
}

// ---- StandardTier ------------------------------------------------------------------------------

StandardTier::StandardTier(const Device& device, const Scene& scene,
                           std::span<const world::Light> lights, std::uint32_t width,
                           std::uint32_t height)
    : device_(&device), scene_(&scene), width_(width), height_(height),
      light_count_(lights.size()) {
    const VkPhysicalDevice physical_device = device.physical_device();
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device, &properties);
    // cs::Image counts pixels in int, so no side may exceed INT_MAX either.
    const std::uint32_t max_side =
        std::min<std::uint32_t>(properties.limits.maxImageDimension2D,
                                static_cast<std::uint32_t>(std::numeric_limits<int>::max()));
    if (width == 0 || height == 0 || width > max_side || height > max_side)
        throw std::invalid_argument("StandardTier: " + std::to_string(width) + "x" +
                                    std::to_string(height) + " is not a size from 1x1 to " +
                                    std::to_string(max_side) + "x" + std::to_string(max_side));

    if (!supports(physical_device, hdr_format,
                  VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                      VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
        throw std::runtime_error("StandardTier: the device cannot render to RGBA32F");
    // Vulkan requires depth attachments in at least one of these two.
    for (const VkFormat format : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32}) {
        if (supports(physical_device, format, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            depth_format_ = format;
            break;
        }
    }
    if (depth_format_ == VK_FORMAT_UNDEFINED)
        throw std::runtime_error("StandardTier: the device has no depth format to render to");
    if (!image_fits(physical_device, hdr_format, hdr_usage, width, height, hdr_texel_bytes) ||
        !image_fits(physical_device, depth_format_, depth_usage, width, height,
                    depth_texel_bytes) ||
        VkDeviceSize{width} * height > detail::max_buffer_bytes(device) / hdr_texel_bytes)
        throw std::invalid_argument("StandardTier: " + std::to_string(width) + "x" +
                                    std::to_string(height) +
                                    " is larger than the device can allocate as one image");

    const LightTable table = make_light_table(lights);
    try {
        create_images();
        upload_lights(table);
        create_descriptors();
        create_pipeline();
    } catch (...) {
        destroy();
        throw;
    }
}

StandardTier::~StandardTier() {
    destroy();
}

void StandardTier::destroy() noexcept {
    const VkDevice device = device_->device();
    const VmaAllocator allocator = device_->allocator();
    vkDestroyPipeline(device, pipeline_, nullptr);
    vkDestroyPipelineLayout(device, layout_, nullptr);
    vkDestroyDescriptorPool(device, pool_, nullptr); // frees the sets
    vkDestroyDescriptorSetLayout(device, set_layout_, nullptr);
    vkDestroySampler(device, sampler_, nullptr);
    if (lights_ != VK_NULL_HANDLE)
        vmaDestroyBuffer(allocator, lights_, lights_allocation_);
    destroy_image(allocator, device, depth_image_, depth_allocation_, depth_view_);
    destroy_image(allocator, device, hdr_image_, hdr_allocation_, hdr_view_);
    destroy_image(allocator, device, white_image_, white_allocation_, white_view_);
}

void StandardTier::create_images() {
    const VkDevice device = device_->device();
    const VmaAllocator allocator = device_->allocator();
    create_image(allocator, device, hdr_format, hdr_usage, width_, height_,
                 VK_IMAGE_ASPECT_COLOR_BIT, hdr_image_, hdr_allocation_, hdr_view_);
    create_image(allocator, device, depth_format_, depth_usage, width_, height_,
                 VK_IMAGE_ASPECT_DEPTH_BIT, depth_image_, depth_allocation_, depth_view_);
    create_image(allocator, device, white_format,
                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1, 1,
                 VK_IMAGE_ASPECT_COLOR_BIT, white_image_, white_allocation_, white_view_);
}

// Copies the lights to a device-local buffer through a staging buffer, and clears the white
// texel, in one submission, after which both are visible to the shaders of later commands.
void StandardTier::upload_lights(const LightTable& table) {
    const VkDeviceSize light_bytes = table.lights.size() * sizeof(LightData);
    const VkDeviceSize profile_bytes = table.profiles.size() * sizeof(float);
    const VkDeviceSize size_bytes = light_bytes + profile_bytes;

    detail::OneTimeCommands commands(*device_);
    const VkCommandBuffer cmd = commands.begin();
    std::optional<detail::HostBuffer> staging;
    if (size_bytes > 0) {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size_bytes;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo allocation_info{};
        allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        check(vmaCreateBuffer(device_->allocator(), &info, &allocation_info, &lights_,
                              &lights_allocation_, nullptr),
              "allocating the lights' buffer", "StandardTier");
        VkBufferDeviceAddressInfo address_info{};
        address_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        address_info.buffer = lights_;
        lights_address_ = vkGetBufferDeviceAddress(device_->device(), &address_info);

        staging.emplace(device_->allocator(), size_bytes, detail::HostBuffer::Use::upload);
        std::memcpy(staging->data(), table.lights.data(), light_bytes);
        std::memcpy(staging->data() + light_bytes, table.profiles.data(), profile_bytes);
        staging->flush();
        const VkBufferCopy region{0, 0, size_bytes};
        vkCmdCopyBuffer(cmd, staging->buffer(), lights_, 1, &region);
    }

    const VkImageMemoryBarrier2 to_clear = detail::image_barrier(
        white_image_, 0, 1, {}, {VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT},
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    detail::record_barriers(cmd, {}, std::span(&to_clear, 1));
    const VkClearColorValue white{{1.0f, 1.0f, 1.0f, 1.0f}};
    const VkImageSubresourceRange colour_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, white_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1,
                         &colour_range);

    constexpr VkPipelineStageFlags2 shaders = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    const VkImageMemoryBarrier2 to_shaders = detail::image_barrier(
        white_image_, 0, 1, {VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT},
        {shaders, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT}, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkMemoryBarrier2 copied{};
    copied.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    copied.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    copied.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    copied.dstStageMask = shaders;
    copied.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    detail::record_barriers(cmd, std::span(&copied, 1), std::span(&to_shaders, 1));
    commands.submit_and_wait();
}

void StandardTier::create_descriptors() {
    const VkDevice device = device_->device();

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    check(vkCreateSampler(device, &sampler_info, nullptr, &sampler_), "creating the sampler",
          "StandardTier");

    // standard.slang: binding 0 the albedo texture, binding 1 the sampler, fixed here.
    const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &sampler_},
    }};
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layout_info.pBindings = bindings.data();
    check(vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &set_layout_),
          "creating the descriptor set layout", "StandardTier");

    // One set per texture, and one with the white texel.
    const std::span<const Texture> textures = scene_->textures();
    const auto set_count = static_cast<std::uint32_t>(textures.size() + 1);
    const std::array<VkDescriptorPoolSize, 2> sizes{{
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, set_count},
        {VK_DESCRIPTOR_TYPE_SAMPLER, set_count},
    }};
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = set_count;
    pool_info.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    pool_info.pPoolSizes = sizes.data();
    check(vkCreateDescriptorPool(device, &pool_info, nullptr, &pool_),
          "creating the descriptor pool", "StandardTier");

    const std::vector<VkDescriptorSetLayout> layouts(set_count, set_layout_);
    std::vector<VkDescriptorSet> sets(set_count);
    VkDescriptorSetAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate_info.descriptorPool = pool_;
    allocate_info.descriptorSetCount = set_count;
    allocate_info.pSetLayouts = layouts.data();
    check(vkAllocateDescriptorSets(device, &allocate_info, sets.data()),
          "allocating the descriptor sets", "StandardTier");
    white_set_ = sets.back();
    texture_sets_.assign(sets.begin(), sets.end() - 1);

    std::vector<VkDescriptorImageInfo> images(set_count);
    std::vector<VkWriteDescriptorSet> writes(set_count);
    for (std::uint32_t i = 0; i < set_count; ++i) {
        images[i].imageView = i < textures.size() ? textures[i].view : white_view_;
        images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = sets[i];
        writes[i].dstBinding = 0;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device, set_count, writes.data(), 0, nullptr);
}

void StandardTier::create_pipeline() {
    const VkDevice device = device_->device();
    const VkPushConstantRange push_range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                         0, sizeof(ForwardPush)};
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    check(vkCreatePipelineLayout(device, &layout_info, nullptr, &layout_),
          "creating the pipeline layout", "StandardTier");

    const std::span<const std::uint32_t> spirv = shader_spirv("standard");
    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = spirv.size_bytes();
    module_info.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &module_info, nullptr, &module),
          "creating the shader module", "StandardTier");

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = module;
    stages[0].pName = "vertex_main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = module;
    stages[1].pName = "fragment_main";

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    // Both sides of every triangle are drawn: both reflect.
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // Reversed depth: nearer is greater.
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_GREATER;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    constexpr std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();
    const VkFormat colour_format = hdr_format;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &colour_format;
    rendering.depthAttachmentFormat = depth_format_;

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.pNext = &rendering;
    pipeline_info.stageCount = static_cast<std::uint32_t>(stages.size());
    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pDepthStencilState = &depth;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = layout_;
    const VkResult result =
        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_);
    // The module is needed only while the pipeline is made.
    vkDestroyShaderModule(device, module, nullptr);
    check(result, "creating the pipeline", "StandardTier");
}

void StandardTier::record(VkCommandBuffer commands, const world::Camera& camera) const {
    ForwardPush push{};
    set_view(push, camera, width_, height_); // throws before anything is recorded
    push.vertices = scene_->vertices().address;
    push.instances = scene_->instances().address;
    push.materials = scene_->materials().address;
    push.lights = lights_address_;
    push.light_count = static_cast<std::uint32_t>(light_count_);

    // Both images start from UNDEFINED, discarding the last frame: after the reads of the HDR
    // image that may follow a frame (a tone-mapping or reduction pass, a readback), and after
    // the last frame's depth writes.
    constexpr VkPipelineStageFlags2 depth_stages =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    constexpr VkPipelineStageFlags2 readers = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                              VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    std::array<VkImageMemoryBarrier2, 2> to_attachments{
        detail::image_barrier(hdr_image_, 0, 1, {readers, VK_ACCESS_2_NONE},
                              {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT},
                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
        detail::image_barrier(depth_image_, 0, 1,
                              {depth_stages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT},
                              {depth_stages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT},
                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL)};
    to_attachments[1].subresourceRange = depth_range;
    detail::record_barriers(commands, {}, to_attachments);

    VkRenderingAttachmentInfo colour{};
    colour.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colour.imageView = hdr_view_;
    colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; // to 0: no surface, no light
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = depth_view_;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; // to 0, the far plane of reversed depth
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {width_, height_}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &colour;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(commands, &rendering);

    const SceneBuffer& indices = scene_->indices();
    if (indices.buffer != VK_NULL_HANDLE) {
        const VkViewport viewport{
            0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, {width_, height_}};
        vkCmdSetViewport(commands, 0, 1, &viewport);
        vkCmdSetScissor(commands, 0, 1, &scissor);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdPushConstants(commands, layout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdBindIndexBuffer(commands, indices.buffer, 0, VK_INDEX_TYPE_UINT32);

        const std::span<const InstanceData> instances = scene_->instance_data();
        const std::span<const MaterialData> materials = scene_->material_data();
        const std::span<const MeshRange> meshes = scene_->meshes();
        VkDescriptorSet bound = VK_NULL_HANDLE;
        for (std::uint32_t i = 0; i < instances.size(); ++i) {
            const InstanceData& instance = instances[i];
            const std::uint32_t index_count = meshes[instance.mesh].index_count;
            if (index_count == 0)
                continue;
            // A painting's texture; the white texel, which the shader does not read, otherwise.
            const MaterialData& material = materials[instance.material];
            const VkDescriptorSet set =
                material.kind == MaterialKind::painting && material.texture < texture_sets_.size()
                    ? texture_sets_[material.texture]
                    : white_set_;
            if (set != bound) {
                vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1,
                                        &set, 0, nullptr);
                bound = set;
            }
            vkCmdDrawIndexed(commands, index_count, 1, instance.first_index,
                             static_cast<std::int32_t>(instance.vertex_offset), i);
        }
    }
    vkCmdEndRendering(commands);

    const VkImageMemoryBarrier2 to_readers = detail::image_barrier(
        hdr_image_, 0, 1,
        {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT},
        {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
         VK_ACCESS_2_SHADER_READ_BIT},
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    detail::record_barriers(commands, {}, std::span(&to_readers, 1));
}

StandardImages StandardTier::render(const world::Camera& camera) const {
    const VkDeviceSize pixels = VkDeviceSize{width_} * height_;
    detail::HostBuffer readback(device_->allocator(), pixels * hdr_texel_bytes,
                                detail::HostBuffer::Use::download);
    detail::OneTimeCommands commands(*device_);
    const VkCommandBuffer cmd = commands.begin();
    record(cmd, camera);

    // The frame's writes, then the copy that reads them.
    const VkImageMemoryBarrier2 to_copy = detail::image_barrier(
        hdr_image_, 0, 1,
        {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT},
        {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT},
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    detail::record_barriers(cmd, {}, std::span(&to_copy, 1));
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width_, height_, 1}; // tightly packed rows
    vkCmdCopyImageToBuffer(cmd, hdr_image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer(),
                           1, &region);
    detail::record_host_read(cmd, readback);
    commands.submit_and_wait();
    readback.invalidate();

    StandardImages images{Image(static_cast<int>(width_), static_cast<int>(height_)), {}};
    images.illuminance_lux.resize(pixels);
    const std::byte* texel = readback.data();
    for (int y = 0; y < images.radiance.height(); ++y) {
        for (int x = 0; x < images.radiance.width(); ++x, texel += hdr_texel_bytes) {
            std::array<float, 4> rgba{};
            std::memcpy(rgba.data(), texel, sizeof(rgba));
            images.radiance.at(x, y) = {rgba[0], rgba[1], rgba[2]};
            images.illuminance_lux[static_cast<std::size_t>(y) * width_ + x] = rgba[3];
        }
    }
    return images;
}

} // namespace cs::gpu
