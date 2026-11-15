// SPDX-License-Identifier: Apache-2.0
#pragma once

// A render target without a window: an RGBA16F colour image and a depth image, drawn into with
// dynamic rendering and read back into a cs::Image. Tests, the command-line renderer and golden
// images render through it, headless on any device, lavapipe included (ADR 0001).

#include <cs/core/image.hpp>
#include <cs/gpu/device.hpp>

#include <array>
#include <cstdint>
#include <functional>

namespace cs::gpu {

// The values the attachments are cleared to before drawing.
struct ClearValues {
    std::array<float, 4> colour{0.0f, 0.0f, 0.0f, 0.0f}; // linear RGBA
    float depth = 1.0f;                                  // the far plane
};

// Owns the images, a host-visible readback buffer, a command pool and a fence, all made once, so
// rendering again allocates nothing. Destroy it before its Device.
//
// The image follows Vulkan's conventions: row 0 is the top, and in clip space y points down, so
// a vertex at y = -1 lands on the top row. cs::Image's row 0 is the top too.
class OffscreenTarget {
  public:
    // Half floats keep linear light above 1 with 11 bits of precision, which is what the tone
    // mapping needs; every Vulkan device can render to and copy from this format.
    static constexpr VkFormat colour_format = VK_FORMAT_R16G16B16A16_SFLOAT;

    // Throws std::invalid_argument when a size is 0, larger than the device's
    // maxImageDimension2D, or too large for the device to allocate the images and the readback
    // buffer, or std::runtime_error on a Vulkan failure.
    OffscreenTarget(const Device& device, std::uint32_t width, std::uint32_t height);
    ~OffscreenTarget();

    OffscreenTarget(const OffscreenTarget&) = delete;
    OffscreenTarget& operator=(const OffscreenTarget&) = delete;
    OffscreenTarget(OffscreenTarget&&) = delete;
    OffscreenTarget& operator=(OffscreenTarget&&) = delete;

    [[nodiscard]] std::uint32_t width() const { return width_; }
    [[nodiscard]] std::uint32_t height() const { return height_; }
    // VK_FORMAT_D32_SFLOAT, or VK_FORMAT_X8_D24_UNORM_PACK32 on a device that cannot render
    // depth to D32 (Vulkan requires one of the two).
    [[nodiscard]] VkFormat depth_format() const { return depth_format_; }

    [[nodiscard]] VkImage colour_image() const { return colour_image_; }
    [[nodiscard]] VkImageView colour_view() const { return colour_view_; }
    [[nodiscard]] VkImage depth_image() const { return depth_image_; }
    [[nodiscard]] VkImageView depth_view() const { return depth_view_; }

    // Called between vkCmdBeginRendering and vkCmdEndRendering, with the viewport and scissor
    // already set to the whole target, to bind pipelines and draw.
    using DrawFunction = std::function<void(VkCommandBuffer)>;

    // Renders one frame and waits for it: clears colour and depth, calls draw, copies the colour
    // image to host memory and returns its RGB (alpha is dropped), width() by height(). Throws
    // std::runtime_error on a Vulkan failure, or when the GPU has not finished within a minute;
    // an exception thrown by draw propagates, and the target stays usable.
    //
    // A pipeline drawn here is made with this target's colour_format and depth_format(), as the
    // attachments are always both bound; GraphicsPipelineOptions::depth_test = false turns the
    // depth test off. After the one-minute timeout the frame may still be running: call
    // Device::wait_idle() before destroying anything draw used.
    [[nodiscard]] Image render(const DrawFunction& draw, const ClearValues& clear = {});

  private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;

    VkImage colour_image_ = VK_NULL_HANDLE;
    VmaAllocation colour_allocation_ = VK_NULL_HANDLE;
    VkImageView colour_view_ = VK_NULL_HANDLE;
    VkImage depth_image_ = VK_NULL_HANDLE;
    VmaAllocation depth_allocation_ = VK_NULL_HANDLE;
    VkImageView depth_view_ = VK_NULL_HANDLE;
    VkBuffer readback_ = VK_NULL_HANDLE;
    VmaAllocation readback_allocation_ = VK_NULL_HANDLE;
    const std::uint16_t* readback_texels_ = nullptr; // mapped, four halves per pixel
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer commands_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    bool pending_ = false; // submitted, and the fence not yet waited for

    // Waits for the submitted frame. Throws as render() does.
    void wait_for_frame();
    void destroy() noexcept;
};

} // namespace cs::gpu
