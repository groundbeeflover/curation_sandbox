// SPDX-License-Identifier: Apache-2.0
#pragma once

// What uploading to and downloading from the GPU share: one command buffer recorded, submitted
// and waited for, and host-visible buffers to stage through. Private to the gpu module.

#include <cs/gpu/device.hpp>

#include <cstddef>

namespace cs::gpu::detail {

// Throws std::runtime_error("<who>: <what> failed (<result>)") unless result is VK_SUCCESS.
void check(VkResult result, const char* who, const char* what);

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
    [[nodiscard]] VkDeviceSize size_bytes() const { return size_bytes_; }

    // After the host's writes, before the copies read them; and after the copies' writes (and
    // the fence), before the host reads. No-ops on host-coherent memory.
    void flush() const;
    void invalidate() const;

  private:
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    std::byte* data_ = nullptr;
    VkDeviceSize size_bytes_ = 0;
};

// The largest buffer the device can allocate as one: the smaller of maxMemoryAllocationSize and
// maxBufferSize.
VkDeviceSize max_buffer_bytes(const Device& device);

// A barrier on one buffer, whole, with the queue family ownership left alone.
VkBufferMemoryBarrier2 buffer_barrier(VkBuffer buffer);
// A barrier on some levels of a colour image, with the queue family ownership left alone.
VkImageMemoryBarrier2 image_barrier(VkImage image, std::uint32_t first_level,
                                    std::uint32_t level_count);
// Records one vkCmdPipelineBarrier2 with these barriers.
void record_barriers(VkCommandBuffer commands, const VkMemoryBarrier2* memory,
                     const VkBufferMemoryBarrier2* buffer, const VkImageMemoryBarrier2* images,
                     std::uint32_t image_count);

} // namespace cs::gpu::detail
