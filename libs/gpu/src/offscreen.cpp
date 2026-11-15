// SPDX-License-Identifier: Apache-2.0

#include "cs/gpu/offscreen.hpp"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>

namespace cs::gpu {

namespace {

// A minute: a frame takes milliseconds, a large one on lavapipe seconds. The bound only keeps a
// lost or hung device from blocking forever.
constexpr std::uint64_t frame_timeout_ns = 60'000'000'000;

constexpr std::size_t channels = 4;                                    // RGBA
constexpr VkDeviceSize texel_bytes = channels * sizeof(std::uint16_t); // half floats

constexpr VkImageUsageFlags colour_usage =
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
constexpr VkImageUsageFlags depth_usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
constexpr VkDeviceSize depth_texel_bytes = 4; // D32_SFLOAT and X8_D24_UNORM_PACK32

constexpr VkImageSubresourceRange colour_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
constexpr VkImageSubresourceRange depth_range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string("OffscreenTarget: ") + what + " failed (" +
                                 std::to_string(static_cast<int>(result)) + ")");
}

bool supports(VkPhysicalDevice physical_device, VkFormat format, VkFormatFeatureFlags features) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physical_device, format, &properties);
    return (properties.optimalTilingFeatures & features) == features;
}

// At most 2^62 for sides below 2^31, so it cannot overflow; byte counts are compared by dividing
// the limit instead of multiplying this.
VkDeviceSize pixels(std::uint32_t width, std::uint32_t height) {
    return VkDeviceSize{width} * height;
}

// Whether the device can make an image this large in this format and use: maxImageDimension2D
// alone is not enough, as a format's maxExtent and maxResourceSize may be smaller.
bool image_fits(VkPhysicalDevice physical_device, VkFormat format, VkImageUsageFlags usage,
                std::uint32_t width, std::uint32_t height, VkDeviceSize texel_size) {
    VkImageFormatProperties properties{};
    if (vkGetPhysicalDeviceImageFormatProperties(physical_device, format, VK_IMAGE_TYPE_2D,
                                                 VK_IMAGE_TILING_OPTIMAL, usage, 0,
                                                 &properties) != VK_SUCCESS)
        return false;
    return width <= properties.maxExtent.width && height <= properties.maxExtent.height &&
           pixels(width, height) <= properties.maxResourceSize / texel_size;
}

VkImageView create_view(VkDevice device, VkImage image, VkFormat format,
                        const VkImageSubresourceRange& range) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange = range;
    VkImageView view = VK_NULL_HANDLE;
    check(vkCreateImageView(device, &info, nullptr, &view), "creating an image view");
    return view;
}

VkImageMemoryBarrier2 image_barrier(VkImage image, const VkImageSubresourceRange& range) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = range;
    return barrier;
}

} // namespace

OffscreenTarget::OffscreenTarget(const Device& device, std::uint32_t width, std::uint32_t height)
    : device_(device.device()), queue_(device.queue()), allocator_(device.allocator()),
      width_(width), height_(height) {
    const VkPhysicalDevice physical_device = device.physical_device();
    VkPhysicalDeviceVulkan11Properties properties_11{};
    properties_11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &properties_11;
    vkGetPhysicalDeviceProperties2(physical_device, &properties);
    // cs::Image counts pixels in int, so no side may exceed INT_MAX either.
    const std::uint32_t max_side =
        std::min<std::uint32_t>(properties.properties.limits.maxImageDimension2D,
                                static_cast<std::uint32_t>(std::numeric_limits<int>::max()));
    if (width == 0 || height == 0 || width > max_side || height > max_side)
        throw std::invalid_argument("OffscreenTarget: " + std::to_string(width) + "x" +
                                    std::to_string(height) + " is not a size from 1x1 to " +
                                    std::to_string(max_side) + "x" + std::to_string(max_side));

    if (!supports(physical_device, colour_format,
                  VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
        throw std::runtime_error("OffscreenTarget: the device cannot render to RGBA16F");
    // Vulkan requires depth attachments in at least one of these two.
    for (const VkFormat format : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32}) {
        if (supports(physical_device, format, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            depth_format_ = format;
            break;
        }
    }
    if (depth_format_ == VK_FORMAT_UNDEFINED)
        throw std::runtime_error("OffscreenTarget: the device can render depth to neither "
                                 "D32_SFLOAT nor X8_D24_UNORM_PACK32");

    // The images and the readback buffer must also fit the format's and the allocator's limits,
    // so that a large size from outside (cs-render --size) is refused here, not by Vulkan.
    if (!image_fits(physical_device, colour_format, colour_usage, width, height, texel_bytes) ||
        !image_fits(physical_device, depth_format_, depth_usage, width, height,
                    depth_texel_bytes) ||
        pixels(width, height) > properties_11.maxMemoryAllocationSize / texel_bytes)
        throw std::invalid_argument("OffscreenTarget: " + std::to_string(width) + "x" +
                                    std::to_string(height) +
                                    " is larger than the device can allocate as one image");

    try {
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.extent = {width, height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo image_allocation{};
        image_allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        image_info.format = colour_format;
        image_info.usage = colour_usage;
        check(vmaCreateImage(allocator_, &image_info, &image_allocation, &colour_image_,
                             &colour_allocation_, nullptr),
              "allocating the colour image");
        colour_view_ = create_view(device_, colour_image_, colour_format, colour_range);

        image_info.format = depth_format_;
        image_info.usage = depth_usage;
        check(vmaCreateImage(allocator_, &image_info, &image_allocation, &depth_image_,
                             &depth_allocation_, nullptr),
              "allocating the depth image");
        depth_view_ = create_view(device_, depth_image_, depth_format_, depth_range);

        // The host reads every texel once, in order, after each frame.
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = pixels(width, height) * texel_bytes;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo buffer_allocation{};
        buffer_allocation.usage = VMA_MEMORY_USAGE_AUTO;
        buffer_allocation.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo mapped{};
        check(vmaCreateBuffer(allocator_, &buffer_info, &buffer_allocation, &readback_,
                              &readback_allocation_, &mapped),
              "allocating the readback buffer");
        if (mapped.pMappedData == nullptr)
            throw std::runtime_error("OffscreenTarget: the readback buffer is not mapped");
        readback_texels_ = static_cast<const std::uint16_t*>(mapped.pMappedData);

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex = device.queue_family();
        check(vkCreateCommandPool(device_, &pool_info, nullptr, &pool_), "creating a command pool");
        VkCommandBufferAllocateInfo command_info{};
        command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_info.commandPool = pool_;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device_, &command_info, &commands_),
              "allocating a command buffer");

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        check(vkCreateFence(device_, &fence_info, nullptr, &fence_), "creating a fence");
    } catch (...) {
        destroy();
        throw;
    }
}

OffscreenTarget::~OffscreenTarget() {
    destroy();
}

void OffscreenTarget::destroy() noexcept {
    // A frame whose wait timed out may still run: nothing it uses may go before it finishes.
    if (pending_)
        vkWaitForFences(device_, 1, &fence_, VK_TRUE, std::numeric_limits<std::uint64_t>::max());
    vkDestroyFence(device_, fence_, nullptr);
    vkDestroyCommandPool(device_, pool_, nullptr); // frees commands_
    if (readback_ != VK_NULL_HANDLE)
        vmaDestroyBuffer(allocator_, readback_, readback_allocation_);
    vkDestroyImageView(device_, depth_view_, nullptr);
    if (depth_image_ != VK_NULL_HANDLE)
        vmaDestroyImage(allocator_, depth_image_, depth_allocation_);
    vkDestroyImageView(device_, colour_view_, nullptr);
    if (colour_image_ != VK_NULL_HANDLE)
        vmaDestroyImage(allocator_, colour_image_, colour_allocation_);
}

void OffscreenTarget::wait_for_frame() {
    const VkResult result = vkWaitForFences(device_, 1, &fence_, VK_TRUE, frame_timeout_ns);
    if (result == VK_TIMEOUT)
        throw std::runtime_error("OffscreenTarget: the GPU did not finish a frame within a minute");
    check(result, "waiting for the frame");
    pending_ = false;
}

Image OffscreenTarget::render(const DrawFunction& draw, const ClearValues& clear) {
    // A previous frame whose wait timed out must finish before the command buffer and the
    // fence are reused.
    if (pending_)
        wait_for_frame();

    // Resetting also ends a recording that an exception from draw left open.
    check(vkResetCommandPool(device_, pool_, 0), "resetting the command pool");
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commands_, &begin_info), "beginning the command buffer");

    // Both attachments start from UNDEFINED, discarding what the last frame left: they are
    // cleared. The source stages order the transitions after the last frame's use of the images
    // (its copy read the colour image, its depth tests wrote the depth image), which the fence
    // wait between frames already does on the host, so that no frame depends on that.
    std::array<VkImageMemoryBarrier2, 2> to_attachments{image_barrier(colour_image_, colour_range),
                                                        image_barrier(depth_image_, depth_range)};
    to_attachments[0].srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_attachments[0].srcAccessMask = VK_ACCESS_2_NONE; // write after read: execution only
    to_attachments[0].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_attachments[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT; // no blending
    to_attachments[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_attachments[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    constexpr VkPipelineStageFlags2 depth_stages =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    to_attachments[1].srcStageMask = depth_stages;
    to_attachments[1].srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    to_attachments[1].dstStageMask = depth_stages;
    to_attachments[1].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    to_attachments[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_attachments[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    VkDependencyInfo to_attachments_dependency{};
    to_attachments_dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    to_attachments_dependency.imageMemoryBarrierCount =
        static_cast<std::uint32_t>(to_attachments.size());
    to_attachments_dependency.pImageMemoryBarriers = to_attachments.data();
    vkCmdPipelineBarrier2(commands_, &to_attachments_dependency);

    VkRenderingAttachmentInfo colour{};
    colour.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colour.imageView = colour_view_;
    colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    for (std::size_t c = 0; c < channels; ++c)
        colour.clearValue.color.float32[c] = clear.colour[c];
    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = depth_view_;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; // depth is not read after the frame
    depth.clearValue.depthStencil.depth = clear.depth;
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {width_, height_}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &colour;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(commands_, &rendering);

    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_),
                              0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {width_, height_}};
    vkCmdSetViewport(commands_, 0, 1, &viewport);
    vkCmdSetScissor(commands_, 0, 1, &scissor);
    if (draw)
        draw(commands_);

    vkCmdEndRendering(commands_);

    // The colour attachment's writes, then the copy that reads it.
    VkImageMemoryBarrier2 to_copy = image_barrier(colour_image_, colour_range);
    to_copy.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_copy.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    to_copy.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_copy.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    to_copy.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkDependencyInfo to_copy_dependency{};
    to_copy_dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    to_copy_dependency.imageMemoryBarrierCount = 1;
    to_copy_dependency.pImageMemoryBarriers = &to_copy;
    vkCmdPipelineBarrier2(commands_, &to_copy_dependency);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width_, height_, 1}; // tightly packed rows
    vkCmdCopyImageToBuffer(commands_, colour_image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback_, 1, &region);

    // The copy's writes, then the host's reads after the fence.
    VkBufferMemoryBarrier2 to_host{};
    to_host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    to_host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.buffer = readback_;
    to_host.size = VK_WHOLE_SIZE;
    VkDependencyInfo to_host_dependency{};
    to_host_dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    to_host_dependency.bufferMemoryBarrierCount = 1;
    to_host_dependency.pBufferMemoryBarriers = &to_host;
    vkCmdPipelineBarrier2(commands_, &to_host_dependency);

    check(vkEndCommandBuffer(commands_), "recording the frame");

    VkCommandBufferSubmitInfo command_submit{};
    command_submit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    command_submit.commandBuffer = commands_;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command_submit;
    // Reset just before the submit, so that a failure anywhere earlier leaves it unsignalled.
    check(vkResetFences(device_, 1, &fence_), "resetting the fence");
    check(vkQueueSubmit2(queue_, 1, &submit, fence_), "submitting the frame");
    pending_ = true;
    wait_for_frame();

    // Mapped memory need not be host-coherent; invalidating is a no-op where it is.
    check(vmaInvalidateAllocation(allocator_, readback_allocation_, 0, VK_WHOLE_SIZE),
          "invalidating the readback buffer");
    Image image(static_cast<int>(width_), static_cast<int>(height_));
    const std::uint16_t* texel = readback_texels_;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x, texel += channels)
            image.at(x, y) = {glm::unpackHalf1x16(texel[0]), glm::unpackHalf1x16(texel[1]),
                              glm::unpackHalf1x16(texel[2])};
    }
    return image;
}

} // namespace cs::gpu
