// SPDX-License-Identifier: Apache-2.0

#include "upload.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace cs::gpu::detail {

namespace {

// A minute, as for a frame (offscreen.cpp): an upload of hundreds of megabytes takes seconds on
// lavapipe. Past it, submit_and_wait throws, but the destructor still waits for the submission
// before it frees anything, so a hung device blocks there; a lost one ends the wait at once.
constexpr std::uint64_t submission_timeout_ns = 60'000'000'000;

} // namespace

void check(VkResult result, const char* what, const char* who) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(who) + ": " + what + " failed (" +
                                 std::to_string(static_cast<int>(result)) + ")");
}

// ---- OneTimeCommands ---------------------------------------------------------------------------

OneTimeCommands::OneTimeCommands(const Device& device)
    : device_(device.device()), queue_(device.queue()) {
    try {
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

OneTimeCommands::~OneTimeCommands() {
    destroy();
}

void OneTimeCommands::destroy() noexcept {
    // A submission whose wait timed out may still run: what it uses must outlive it.
    if (pending_)
        vkWaitForFences(device_, 1, &fence_, VK_TRUE, std::numeric_limits<std::uint64_t>::max());
    vkDestroyFence(device_, fence_, nullptr);
    vkDestroyCommandPool(device_, pool_, nullptr); // frees commands_
}

VkCommandBuffer OneTimeCommands::begin() {
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commands_, &begin_info), "beginning a command buffer");
    return commands_;
}

void OneTimeCommands::submit_and_wait() {
    check(vkEndCommandBuffer(commands_), "recording commands");
    VkCommandBufferSubmitInfo command_submit{};
    command_submit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    command_submit.commandBuffer = commands_;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command_submit;
    check(vkQueueSubmit2(queue_, 1, &submit, fence_), "submitting commands");
    pending_ = true;
    const VkResult result = vkWaitForFences(device_, 1, &fence_, VK_TRUE, submission_timeout_ns);
    if (result == VK_TIMEOUT)
        throw std::runtime_error("gpu: the GPU did not finish an upload or download within a "
                                 "minute");
    check(result, "waiting for commands");
    pending_ = false;
}

// ---- HostBuffer --------------------------------------------------------------------------------

HostBuffer::HostBuffer(VmaAllocator allocator, VkDeviceSize size_bytes, Use use)
    : allocator_(allocator) {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size_bytes;
    buffer_info.usage =
        use == Use::upload ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
    allocation_info.flags =
        VMA_ALLOCATION_CREATE_MAPPED_BIT |
        (use == Use::upload ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                            : VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
    VmaAllocationInfo mapped{};
    check(vmaCreateBuffer(allocator_, &buffer_info, &allocation_info, &buffer_, &allocation_,
                          &mapped),
          use == Use::upload ? "allocating a staging buffer" : "allocating a readback buffer");
    data_ = static_cast<std::byte*>(mapped.pMappedData);
    if (data_ == nullptr) {
        vmaDestroyBuffer(allocator_, buffer_, allocation_);
        throw std::runtime_error("gpu: a host-visible buffer is not mapped");
    }
}

HostBuffer::~HostBuffer() {
    vmaDestroyBuffer(allocator_, buffer_, allocation_);
}

void HostBuffer::flush() const {
    check(vmaFlushAllocation(allocator_, allocation_, 0, VK_WHOLE_SIZE),
          "flushing a staging buffer");
}

void HostBuffer::invalidate() const {
    check(vmaInvalidateAllocation(allocator_, allocation_, 0, VK_WHOLE_SIZE),
          "invalidating a readback buffer");
}

// ---- Limits and barriers -----------------------------------------------------------------------

VkDeviceSize max_buffer_bytes(const Device& device) {
    VkPhysicalDeviceVulkan13Properties properties_13{};
    properties_13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES;
    VkPhysicalDeviceVulkan11Properties properties_11{};
    properties_11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;
    properties_11.pNext = &properties_13;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &properties_11;
    vkGetPhysicalDeviceProperties2(device.physical_device(), &properties);
    return std::min(properties_11.maxMemoryAllocationSize, properties_13.maxBufferSize);
}

VkImageMemoryBarrier2 image_barrier(VkImage image, std::uint32_t first_level,
                                    std::uint32_t level_count, Access from, Access to,
                                    VkImageLayout old_layout, VkImageLayout new_layout) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = from.stages;
    barrier.srcAccessMask = from.accesses;
    barrier.dstStageMask = to.stages;
    barrier.dstAccessMask = to.accesses;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, first_level, level_count, 0, 1};
    return barrier;
}

void record_barriers(VkCommandBuffer commands, std::span<const VkMemoryBarrier2> memory,
                     std::span<const VkImageMemoryBarrier2> images) {
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = static_cast<std::uint32_t>(memory.size());
    dependency.pMemoryBarriers = memory.data();
    dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(images.size());
    dependency.pImageMemoryBarriers = images.data();
    vkCmdPipelineBarrier2(commands, &dependency);
}

void record_host_read(VkCommandBuffer commands, const HostBuffer& readback) {
    VkBufferMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = readback.buffer();
    barrier.size = VK_WHOLE_SIZE;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commands, &dependency);
}

} // namespace cs::gpu::detail
