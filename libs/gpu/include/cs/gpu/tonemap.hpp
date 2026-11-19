// SPDX-License-Identifier: Apache-2.0
#pragma once

// The tone-mapping pass
// ---------------------
// Turns an HDR image of StandardTier::hdr_format (RGB radiance in nits, A illuminance in lux)
// into the display image: each pixel's radiance times exposure_scale(ev100), through PBR Neutral,
// then sRGB-encoded by the display image's _SRGB format, exactly as libs/core's CPU pipeline does
// (cs/core/colour.hpp, then linear_to_srgb8). The shaders' port of libs/core is
// shaders/modules/colour.slang. The illuminance is never read: the display image is opaque.
//
// The display image has the HDR image's size and reads one HDR texel per pixel, with no
// filtering. M1-23's presenter blits it into an _SRGB swapchain image: a blit between _SRGB
// formats decodes the colours and encodes them again, which at the same size leaves them as they
// were. A UNORM swapchain would show the encoded values as if they were linear: too dark.

#include <cs/gpu/device.hpp>
#include <cs/gpu/standard_tier.hpp>

#include <cstdint>
#include <vector>

namespace cs::gpu {

// Owns the display image, its pipeline and its descriptor set. It refers to the HDR image's view,
// which must outlive it (StandardTier::hdr_view(): a size change means a new tier and a new pass).
// Destroy it before its Device, and only once every submission of what record() recorded has
// finished: it frees everything at once, without waiting.
class ToneMapPass {
  public:
    // 8-bit sRGB, encoded by the hardware when the pass writes it.
    static constexpr VkFormat display_format = VK_FORMAT_R8G8B8A8_SRGB;

    // The largest EV100, either way, that record() takes: far beyond any scene (libs/core meters
    // from -10.3 to 36.2), and well inside float's range for the shader's 2^EV100, which
    // overflows beyond 128.
    static constexpr double max_ev100_magnitude = 100.0;

    // hdr_view: a 2D view of a width x height image of StandardTier::hdr_format, made with
    // VK_IMAGE_USAGE_SAMPLED_BIT. Throws std::invalid_argument for a null view or a size that is
    // 0 or larger than the device can render to (maxImageDimension2D, maxFramebufferWidth and
    // maxFramebufferHeight), std::runtime_error on a Vulkan failure.
    ToneMapPass(const Device& device, VkImageView hdr_view, std::uint32_t width,
                std::uint32_t height);
    // The same for the tier's HDR image, at its size, so that the sizes cannot disagree: a pass
    // larger than its HDR image would read beyond it. The tier must outlive the pass.
    ToneMapPass(const Device& device, const StandardTier& tier)
        : ToneMapPass(device, tier.hdr_view(), tier.width(), tier.height()) {}
    ~ToneMapPass();

    ToneMapPass(const ToneMapPass&) = delete;
    ToneMapPass& operator=(const ToneMapPass&) = delete;
    ToneMapPass(ToneMapPass&&) = delete;
    ToneMapPass& operator=(ToneMapPass&&) = delete;

    [[nodiscard]] std::uint32_t width() const { return width_; }
    [[nodiscard]] std::uint32_t height() const { return height_; }
    [[nodiscard]] VkImage display_image() const { return display_image_; }
    [[nodiscard]] VkImageView display_view() const { return display_view_; }

    // Records the pass at this EV100 into a command buffer that is recording, outside any
    // rendering. The HDR image must be in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL with its writes
    // visible to fragment shader reads, as StandardTier::record() leaves it; the pass adds no
    // barrier on it. Before it, the pass waits for copies and blits of the display image by
    // earlier commands on the queue, and discards what the image held. After it, the display
    // image is in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, its writes visible to copies and blits of
    // later commands. Throws std::invalid_argument, before recording anything, for an EV100 that
    // is not finite or is beyond max_ev100_magnitude.
    void record(VkCommandBuffer commands, double ev100) const;

    // Records the pass as above, submits it to the device's queue, waits up to a minute and reads
    // the display image back: width() * height() pixels of 4 bytes, R, G, B (sRGB-encoded) and
    // A = 255, row by row from the top. Leaves the display image as record() does. Throws as
    // record() does, or std::runtime_error on a Vulkan failure or the timeout. It submits to the
    // device's queue, so not while another thread uses that queue.
    [[nodiscard]] std::vector<std::uint8_t> render(double ev100) const;

  private:
    const Device* device_ = nullptr;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;

    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE; // the HDR image
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkImage display_image_ = VK_NULL_HANDLE;
    VmaAllocation display_allocation_ = VK_NULL_HANDLE;
    VkImageView display_view_ = VK_NULL_HANDLE;

    void create_descriptors(VkImageView hdr_view);
    void create_pipeline();
    void destroy() noexcept;
};

} // namespace cs::gpu
