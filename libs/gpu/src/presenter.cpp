// SPDX-License-Identifier: Apache-2.0

#include "cs/gpu/presenter.hpp"

#include "upload.hpp"

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace cs::gpu {

using detail::Access;
using detail::check;
using detail::image_barrier;

namespace {

constexpr const char* who = "Presenter";

// A minute, as for an offscreen frame: only a lost or hung device takes that long.
constexpr std::uint64_t timeout_ns = 60'000'000'000;

// The swapchain image is only ever touched by transfers: the acquire's wait, the blit, the
// read-back copy and the signal for the present all name this stage, so that the barriers chain.
constexpr VkPipelineStageFlags2 transfer = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;

constexpr std::uint32_t undefined_extent = std::numeric_limits<std::uint32_t>::max();

bool has_features(VkPhysicalDevice physical_device, VkFormat format,
                  VkFormatFeatureFlags features) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physical_device, format, &properties);
    return (properties.optimalTilingFeatures & features) == features;
}

void record_barriers(VkCommandBuffer commands,
                     std::initializer_list<VkImageMemoryBarrier2> images) {
    detail::record_barriers(commands, {}, {images.begin(), images.size()});
}

} // namespace

std::optional<VkSurfaceFormatKHR>
choose_surface_format(std::span<const VkSurfaceFormatKHR> offered,
                      const std::function<bool(VkFormat)>& can_blit_to) {
    for (const VkFormat format : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB}) {
        for (const VkSurfaceFormatKHR& candidate : offered) {
            if (candidate.format == format &&
                candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && can_blit_to(format))
                return candidate;
        }
    }
    return std::nullopt;
}

Presenter::Presenter(const Device& device, VkSurfaceKHR surface, std::uint32_t width,
                     std::uint32_t height)
    : device_(device.device()), physical_device_(device.physical_device()), queue_(device.queue()),
      allocator_(device.allocator()), surface_(surface), wanted_{width, height} {
    if (!device.presentation())
        throw std::invalid_argument("Presenter: the device was made without "
                                    "DeviceOptions::presentation");
    if (!device.can_present(surface))
        throw std::invalid_argument("Presenter: the device's queue cannot present to the surface");

    std::uint32_t count = 0;
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &count, nullptr),
          "listing the surface's formats", who);
    std::vector<VkSurfaceFormatKHR> offered(count);
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &count, offered.data()),
          "listing the surface's formats", who);
    offered.resize(count);
    const auto chosen = choose_surface_format(offered, [&](VkFormat format) {
        return has_features(physical_device_, format, VK_FORMAT_FEATURE_BLIT_DST_BIT);
    });
    if (!chosen)
        throw std::runtime_error(
            "Presenter: the surface offers neither B8G8R8A8_SRGB nor R8G8B8A8_SRGB (in the sRGB "
            "colour space, as a blit destination), so the display image would be shown encoded "
            "twice or not at all");
    surface_format_ = *chosen;

    VkSurfaceCapabilitiesKHR capabilities{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &capabilities),
          "reading the surface's capabilities", who);
    if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
        throw std::runtime_error("Presenter: the surface's images cannot be blitted to");
    usage_ = capabilities.supportedUsageFlags &
             (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    try {
        for (Frame& frame : frames_) {
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pool_info.queueFamilyIndex = device.queue_family();
            check(vkCreateCommandPool(device_, &pool_info, nullptr, &frame.pool),
                  "creating a command pool", who);
            VkCommandBufferAllocateInfo command_info{};
            command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            command_info.commandPool = frame.pool;
            command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            command_info.commandBufferCount = 1;
            check(vkAllocateCommandBuffers(device_, &command_info, &frame.commands),
                  "allocating a command buffer", who);
            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            check(vkCreateFence(device_, &fence_info, nullptr, &frame.done), "creating a fence",
                  who);
            VkSemaphoreCreateInfo semaphore_info{};
            semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            check(vkCreateSemaphore(device_, &semaphore_info, nullptr, &frame.acquired),
                  "creating a semaphore", who);
        }
        make_swapchain();
    } catch (...) {
        destroy();
        throw;
    }
}

Presenter::~Presenter() {
    destroy();
}

void Presenter::destroy() noexcept {
    // Every frame, and the presents waiting on their semaphores, must finish first.
    vkDeviceWaitIdle(device_);
    read_back_.reset();
    for (const VkSemaphore semaphore : rendered_)
        vkDestroySemaphore(device_, semaphore, nullptr);
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    for (const Frame& frame : frames_) {
        vkDestroySemaphore(device_, frame.acquired, nullptr);
        vkDestroyFence(device_, frame.done, nullptr);
        vkDestroyCommandPool(device_, frame.pool, nullptr); // frees frame.commands
    }
}

bool Presenter::can_read_back() const {
    return (usage_ & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
}

void Presenter::resize(std::uint32_t width, std::uint32_t height) {
    if (width != wanted_.width || height != wanted_.height) {
        wanted_ = {width, height};
        stale_ = true;
    }
}

// Makes the swapchain again when it is stale; false when the window has no area to show.
bool Presenter::make_swapchain() {
    if (swapchain_ != VK_NULL_HANDLE && !stale_)
        return true;
    // The old swapchain's images may still be in a frame or a present. Waiting first also reads
    // the surface as late as possible, while a window may still be changing size.
    check(vkDeviceWaitIdle(device_), "waiting for the device", who);
    VkSurfaceCapabilitiesKHR capabilities{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &capabilities),
          "reading the surface's capabilities", who);
    // Wayland and headless surfaces leave the size to the swapchain; others fix it (a minimised
    // window on Windows at 0 by 0).
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == undefined_extent)
        extent = {std::clamp(wanted_.width, capabilities.minImageExtent.width,
                             capabilities.maxImageExtent.width),
                  std::clamp(wanted_.height, capabilities.minImageExtent.height,
                             capabilities.maxImageExtent.height)};
    if (wanted_.width == 0 || wanted_.height == 0 || extent.width == 0 || extent.height == 0)
        return false;

    // One image more than the minimum, so that acquiring rarely waits for the display.
    std::uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount != 0)
        image_count = std::min(image_count, capabilities.maxImageCount);
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((capabilities.supportedCompositeAlpha & alpha) == 0) // the lowest bit offered
        alpha = static_cast<VkCompositeAlphaFlagBitsKHR>(
            capabilities.supportedCompositeAlpha & ~(capabilities.supportedCompositeAlpha - 1));

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_;
    info.minImageCount = image_count;
    info.imageFormat = surface_format_.format;
    info.imageColorSpace = surface_format_.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = usage_;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR; // the one every surface offers
    info.clipped = VK_FALSE; // clipped pixels would read back undefined in a screenshot
    info.oldSwapchain = swapchain_;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    const VkResult created = vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain);
    // The old swapchain is retired even when that failed, and a retired one may not be passed
    // again: it goes either way, so the next frame starts from none.
    vkDestroySwapchainKHR(device_, std::exchange(swapchain_, swapchain), nullptr);
    if (created != VK_SUCCESS) {
        images_.clear();
        extent_ = {};
    }
    check(created, "creating a swapchain", who);
    extent_ = extent;
    ++swapchains_created_;

    std::uint32_t count = 0;
    check(vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr),
          "listing the swapchain's images", who);
    images_.resize(count);
    check(vkGetSwapchainImagesKHR(device_, swapchain_, &count, images_.data()),
          "listing the swapchain's images", who);
    for (const VkSemaphore semaphore : std::exchange(rendered_, {}))
        vkDestroySemaphore(device_, semaphore, nullptr);
    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (std::uint32_t i = 0; i < count; ++i) {
        check(vkCreateSemaphore(device_, &semaphore_info, nullptr, &rendered_.emplace_back()),
              "creating a semaphore", who);
    }
    stale_ = false;
    return true;
}

bool Presenter::present(const PresentSource& source) {
    return present_frame(source, false);
}

std::optional<PresentedImage> Presenter::present_and_read_back(const PresentSource& source) {
    if (!can_read_back())
        throw std::logic_error("Presenter: the surface's images cannot be copied back");
    const std::uint32_t frame = frame_;
    if (!present_frame(source, true))
        return std::nullopt;
    const VkResult waited = vkWaitForFences(device_, 1, &frames_[frame].done, VK_TRUE, timeout_ns);
    if (waited == VK_TIMEOUT)
        throw std::runtime_error("Presenter: the GPU did not finish a frame within a minute");
    check(waited, "waiting for a frame", who);
    read_back_->invalidate();

    PresentedImage image{extent_.width, extent_.height, {}};
    const std::byte* bytes = read_back_->data();
    image.rgba.resize(std::size_t{4} * extent_.width * extent_.height);
    const bool bgra = surface_format_.format == VK_FORMAT_B8G8R8A8_SRGB;
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        for (std::size_t c = 0; c < 4; ++c) {
            const std::size_t from = bgra && c != 3 ? 2 - c : c;
            image.rgba[i + c] = static_cast<std::uint8_t>(bytes[i + from]);
        }
    }
    return image;
}

bool Presenter::present_frame(const PresentSource& source, bool read_back) {
    if (source.image == VK_NULL_HANDLE || source.extent.width == 0 || source.extent.height == 0 ||
        source.layout == VK_IMAGE_LAYOUT_UNDEFINED ||
        source.layout == VK_IMAGE_LAYOUT_PREINITIALIZED ||
        source.stages == VK_PIPELINE_STAGE_2_NONE)
        throw std::invalid_argument("Presenter: the source has no image, no area, no layout or no "
                                    "stages to wait for");
    // RGBA32F, the HDR image, need not be linearly filterable: it is no display image.
    if (source.format != blit_source_format_) {
        if (!has_features(physical_device_, source.format,
                          VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
            throw std::invalid_argument("Presenter: the device cannot blit from the source's "
                                        "format with a linear filter");
        blit_source_format_ = source.format;
    }

    // Twice: a surface that changes between making the swapchain and acquiring from it gets one
    // more swapchain, then the frame is skipped.
    std::uint32_t index = 0;
    Frame& frame = frames_[frame_];
    for (int attempt = 0;; ++attempt) {
        if (attempt == 2 || !make_swapchain())
            return false;
        // The frame two before this one used the same command buffer and semaphore.
        const VkResult waited = vkWaitForFences(device_, 1, &frame.done, VK_TRUE, timeout_ns);
        if (waited == VK_TIMEOUT)
            throw std::runtime_error("Presenter: the GPU did not finish a frame within a minute");
        check(waited, "waiting for a frame", who);
        // Allocated before the acquire, so that a failure here leaves no image acquired and no
        // semaphore signal pending. The extent is final: only make_swapchain() changes it.
        const VkDeviceSize read_back_bytes = VkDeviceSize{4} * extent_.width * extent_.height;
        if (read_back && (!read_back_ || read_back_bytes_ != read_back_bytes)) {
            read_back_.reset();
            read_back_ = std::make_unique<detail::HostBuffer>(allocator_, read_back_bytes,
                                                              detail::HostBuffer::Use::download);
            read_back_bytes_ = read_back_bytes;
        }
        const VkResult acquired = vkAcquireNextImageKHR(device_, swapchain_, timeout_ns,
                                                        frame.acquired, VK_NULL_HANDLE, &index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            stale_ = true;
            continue;
        }
        if (acquired == VK_TIMEOUT || acquired == VK_NOT_READY)
            return false; // a hidden window's images may be held: nothing was acquired
        if (acquired == VK_SUBOPTIMAL_KHR)
            stale_ = true; // still presentable: this frame goes on, the next makes a new one
        else
            check(acquired, "acquiring a swapchain image", who);
        break;
    }

    check(vkResetCommandPool(device_, frame.pool, 0), "resetting a command pool", who);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(frame.commands, &begin), "beginning a command buffer", who);
    const VkCommandBuffer commands = frame.commands;
    const VkImage target = images_[index];

    // The source after its writes; the swapchain image after the acquire, its contents discarded.
    const Access source_writes{source.stages, source.accesses};
    const Access blit_read{VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
    const Access blit_write{VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
    record_barriers(
        commands, {image_barrier(source.image, 0, 1, source_writes, blit_read, source.layout,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                   image_barrier(target, 0, 1, {transfer, VK_ACCESS_2_NONE}, blit_write,
                                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)});

    // Between two _SRGB formats the blit decodes and encodes again: the colours are unchanged.
    VkImageBlit2 region{};
    region.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2;
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[1] = {static_cast<std::int32_t>(source.extent.width),
                            static_cast<std::int32_t>(source.extent.height), 1};
    region.dstSubresource = region.srcSubresource;
    region.dstOffsets[1] = {static_cast<std::int32_t>(extent_.width),
                            static_cast<std::int32_t>(extent_.height), 1};
    VkBlitImageInfo2 blit{};
    blit.sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2;
    blit.srcImage = source.image;
    blit.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    blit.dstImage = target;
    blit.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    blit.regionCount = 1;
    blit.pRegions = &region;
    blit.filter = VK_FILTER_LINEAR; // a source of another size is stretched smoothly
    vkCmdBlitImage2(commands, &blit);

    Access target_last = blit_write;
    VkImageLayout target_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (read_back) {
        const Access copy_read{VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
        record_barriers(commands, {image_barrier(target, 0, 1, blit_write, copy_read, target_layout,
                                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)});
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {extent_.width, extent_.height, 1}; // tightly packed rows
        vkCmdCopyImageToBuffer(commands, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               read_back_->buffer(), 1, &copy);
        detail::record_host_read(commands, *read_back_);
        target_last = {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_NONE};
        target_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    // The swapchain image to the present, before the semaphore it waits for; the source back to
    // its layout, before the stages that write it next.
    record_barriers(
        commands,
        {image_barrier(target, 0, 1, target_last, {transfer, VK_ACCESS_2_NONE}, target_layout,
                       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
         image_barrier(source.image, 0, 1, {VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_NONE},
                       source_writes, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source.layout)});
    check(vkEndCommandBuffer(commands), "recording a frame", who);

    VkSemaphoreSubmitInfo wait{};
    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait.semaphore = frame.acquired;
    wait.stageMask = transfer;
    VkSemaphoreSubmitInfo signal{};
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal.semaphore = rendered_[index];
    signal.stageMask = transfer;
    VkCommandBufferSubmitInfo command_submit{};
    command_submit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    command_submit.commandBuffer = commands;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command_submit;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    check(vkResetFences(device_, 1, &frame.done), "resetting a fence", who);
    check(vkQueueSubmit2(queue_, 1, &submit, frame.done), "submitting a frame", who);
    frame_ = (frame_ + 1) % frames_in_flight;

    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &rendered_[index];
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain_;
    present_info.pImageIndices = &index;
    const VkResult presented = vkQueuePresentKHR(queue_, &present_info);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
        stale_ = true; // the image was shown or dropped; the next frame makes a new swapchain
    else
        check(presented, "presenting", who);
    return true;
}

} // namespace cs::gpu
