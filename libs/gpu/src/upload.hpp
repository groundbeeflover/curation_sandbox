// SPDX-License-Identifier: Apache-2.0
#pragma once

// What uploading to and downloading from the GPU share: one command buffer recorded, submitted
// and waited for, and host-visible buffers to stage through. Private to the gpu module.

#include <cs/gpu/device.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace cs::gpu::detail {

// Throws std::runtime_error("<who>: <what> failed (<result>)") unless result is VK_SUCCESS.
void check(VkResult result, const char* what, const char* who = "gpu");

// One primary command buffer from its own transient pool, and a fence: begin(), record,
// submit_and_wait(). The destructor waits for a submission whose wait timed out before it frees
// anything, so nothing the commands use may be destroyed before this object.
class OneTimeCommands {
  public:
    explicit OneTimeCommands(const Device& device);
    ~OneTimeCommands();

    OneTimeCommands(const OneTimeCommands&) = delete;
    OneTimeCommands& operator=(const OneTimeCommands&) = delete;
    OneTimeCommands(OneTimeCommands&&) = delete;
    OneTimeCommands& operator=(OneTimeCommands&&) = delete;

    // The command buffer, recording. Call once.
    VkCommandBuffer begin();
    // Ends the recording, submits it to the device's queue and waits up to a minute. Throws
    // std::runtime_error on a Vulkan failure or a timeout.
    void submit_and_wait();

  private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer commands_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    bool pending_ = false; // submitted, and not yet seen to finish

    void destroy() noexcept;
};

// A host-visible buffer, persistently mapped, freed with the object.
class HostBuffer {
  public:
    enum class Use {
        upload,   // written once in order by the host, read by copies: a staging buffer
        download, // written by copies, read by the host
    };

    // Throws std::runtime_error on a Vulkan failure. size_bytes must not be 0.
    HostBuffer(VmaAllocator allocator, VkDeviceSize size_bytes, Use use);
    ~HostBuffer();

    HostBuffer(const HostBuffer&) = delete;
    HostBuffer& operator=(const HostBuffer&) = delete;
    HostBuffer(HostBuffer&&) = delete;
    HostBuffer& operator=(HostBuffer&&) = delete;

    [[nodiscard]] VkBuffer buffer() const { return buffer_; }
    [[nodiscard]] std::byte* data() const { return data_; }

    // After the host's writes, before the copies read them; and after the copies' writes (and
    // the fence), before the host reads. No-ops on host-coherent memory.
    void flush() const;
    void invalidate() const;

  private:
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    std::byte* data_ = nullptr;
};

// The largest buffer the device can allocate as one: the smaller of maxMemoryAllocationSize and
// maxBufferSize.
VkDeviceSize max_buffer_bytes(const Device& device);

// One side of a barrier: the stages, and the accesses in them, that it waits for or holds back.
struct Access {
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 accesses = VK_ACCESS_2_NONE;
};

// A barrier on some levels of a colour image, from one layout to another, with the queue family
// ownership left alone.
VkImageMemoryBarrier2 image_barrier(VkImage image, std::uint32_t first_level,
                                    std::uint32_t level_count, Access from, Access to,
                                    VkImageLayout old_layout, VkImageLayout new_layout);

// Records one vkCmdPipelineBarrier2 with these barriers.
void record_barriers(VkCommandBuffer commands, std::span<const VkMemoryBarrier2> memory,
                     std::span<const VkImageMemoryBarrier2> images);

// Records the barrier that makes a copy's writes to a readback buffer visible to the host after
// the submission's fence.
void record_host_read(VkCommandBuffer commands, const HostBuffer& readback);

} // namespace cs::gpu::detail
