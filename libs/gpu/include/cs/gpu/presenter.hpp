// SPDX-License-Identifier: Apache-2.0
#pragma once

// Shows rendered frames in a window. The presenter owns a swapchain on a VkSurfaceKHR that the
// window's owner made (Qt in the app, a headless surface in tests) and blits an image into it each
// frame; it knows nothing of the toolkit (ADR 0006). The window says its size in pixels with
// resize(), and the swapchain is made again at the next frame after that, and whenever the surface
// reports it out of date or suboptimal (ADR 0011).
//
// The image shown is the display image (M1-44): already tone mapped, VK_FORMAT_R8G8B8A8_SRGB, so
// its sRGB encoding is done by its format. The swapchain has an _SRGB format too, so the blit
// decodes and encodes again and the colours arrive unchanged, encoded once.

#include <cs/gpu/device.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace cs::gpu {

namespace detail {
class HostBuffer;
}

// The image a frame shows, drawn by commands submitted earlier to the device's queue: optimal
// tiling, with TRANSFER_SRC usage. The blit takes its texels as linear values: an _SRGB format is
// decoded, any other is read as it is, so the display image must be _SRGB (or float) for its
// colours to arrive unchanged.
struct PresentSource {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_R8G8B8A8_SRGB; // one the device can blit from, linearly filtered
    VkExtent2D extent{};                       // stretched to the swapchain's when they differ
    // The layout it is in, the stages that use it, and every access those stages make to it,
    // reads included. The presenter waits for them, reads the image in TRANSFER_SRC_OPTIMAL, and
    // puts it back in this layout, made visible to those stages before they run again. So the
    // producer's first use of it in the next frame must be in these stages, or a change of layout
    // of its own; with M1-44's tone-mapping pass, which leaves it in TRANSFER_SRC_OPTIMAL and
    // discards it next frame, COLOR_ATTACHMENT_OUTPUT and COLOR_ATTACHMENT_WRITE.
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 accesses = VK_ACCESS_2_NONE;
};

// A presented frame copied back from the swapchain: 8-bit sRGB-encoded bytes as the swapchain
// holds them, four per pixel in R, G, B, A order whatever the swapchain's order, row 0 at the top.
struct PresentedImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

// The swapchain format among a surface's: B8G8R8A8_SRGB, else R8G8B8A8_SRGB, in the sRGB
// non-linear colour space and only where can_blit_to says the device can blit to it. Nothing when
// the surface offers neither: the presenter then refuses the surface rather than show an image
// encoded twice or not at all.
[[nodiscard]] std::optional<VkSurfaceFormatKHR>
choose_surface_format(std::span<const VkSurfaceFormatKHR> offered,
                      const std::function<bool(VkFormat)>& can_blit_to);

// Two frames in flight: the CPU records one while the GPU blits the other. Destroy it before its
// Device, and the surface after it.
class Presenter {
  public:
    static constexpr std::uint32_t frames_in_flight = 2;

    // A size of 0 is a window with no area (minimised): frames are skipped until it has one.
    // Throws std::invalid_argument when the device was made without DeviceOptions::presentation
    // or its queue cannot present to the surface, and std::runtime_error when the surface offers
    // no sRGB format the device can blit to, cannot take transfers into its images, or on a
    // Vulkan failure.
    Presenter(const Device& device, VkSurfaceKHR surface, std::uint32_t width,
              std::uint32_t height);
    ~Presenter();

    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;
    Presenter(Presenter&&) = delete;
    Presenter& operator=(Presenter&&) = delete;

    // The window's new size in pixels; the swapchain follows at the next frame. Where the surface
    // fixes its own size (X11, Windows) that size wins; where it does not (Wayland, headless),
    // this one is used, clamped to the surface's limits.
    void resize(std::uint32_t width, std::uint32_t height);

    // Blits the source into the next swapchain image and presents it. Returns false when no frame
    // was shown: the window has no area, the surface changed twice in a row, or no swapchain
    // image was free within a minute; present again next frame. Throws std::invalid_argument for
    // a source with no image, no area, an undefined layout, no stages, or a format the device
    // cannot blit from with a linear filter. Throws std::runtime_error on a Vulkan failure or
    // when the GPU has not finished a frame within a minute; after a std::runtime_error the
    // presenter may only be destroyed.
    bool present(const PresentSource& source);

    // As present(), then waits for the frame and copies the presented image back (for
    // screenshots, ADR 0011). Nothing when no frame was shown. Throws std::logic_error when
    // can_read_back() is false.
    std::optional<PresentedImage> present_and_read_back(const PresentSource& source);

    // The swapchain's size, {0, 0} before the first one; its format, B8G8R8A8_SRGB or
    // R8G8B8A8_SRGB; whether its images can be copied back; and how many swapchains have been
    // made (the first, then one for each change of size or surface).
    [[nodiscard]] VkExtent2D extent() const { return extent_; }
    [[nodiscard]] VkFormat format() const { return surface_format_.format; }
    [[nodiscard]] bool can_read_back() const;
    [[nodiscard]] std::uint32_t swapchains_created() const { return swapchains_created_; }

  private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkFence done = VK_NULL_HANDLE;         // created signalled
        VkSemaphore acquired = VK_NULL_HANDLE; // the swapchain image may be written
    };

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSurfaceFormatKHR surface_format_{};
    VkImageUsageFlags usage_ = 0;

    VkExtent2D wanted_{};
    VkExtent2D extent_{};
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> images_;
    std::vector<VkSemaphore> rendered_; // one per swapchain image, signalled for its present
    bool stale_ = true;                 // made again at the next frame
    std::uint32_t swapchains_created_ = 0;

    std::array<Frame, frames_in_flight> frames_{};
    std::uint32_t frame_ = 0;
    std::unique_ptr<detail::HostBuffer> read_back_; // kept for the next read-back of its size
    VkDeviceSize read_back_bytes_ = 0;
    VkFormat blit_source_format_ = VK_FORMAT_UNDEFINED; // the last source format found blittable

    bool make_swapchain();
    bool present_frame(const PresentSource& source, bool read_back);
    void destroy() noexcept;
};

} // namespace cs::gpu
