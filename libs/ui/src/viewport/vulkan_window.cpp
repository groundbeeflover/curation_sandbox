// SPDX-License-Identifier: Apache-2.0
#include "viewport/vulkan_window.hpp"

#include "viewport/gpu_context.hpp"
#include "viewport/view_renderer.hpp"
#include "viewport/viewport.hpp"

#include <QEvent>
#include <QPlatformSurfaceEvent>

#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace cs::ui {

namespace {

// Frames present() may skip in a row before the window stops asking for more: a hidden window
// can make each one wait 100 ms for a swapchain image.
constexpr int max_skipped_frames = 3;

// A minute, as the GPU module waits for its own frames.
constexpr std::uint64_t fence_timeout_ns = 60'000'000'000;

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " failed: VkResult " +
                                 std::to_string(static_cast<int>(result)));
    }
}

} // namespace

VulkanWindow::VulkanWindow(Viewport& viewport)
    : viewport_(&viewport), device_(&viewport.gpu_->device()) {
    setSurfaceType(QSurface::VulkanSurface);
    instance_.setVkInstance(device_->instance());
    if (!instance_.create()) {
        throw std::runtime_error("Qt cannot use the Vulkan instance (QVulkanInstance::create "
                                 "failed: " +
                                 std::to_string(static_cast<int>(instance_.errorCode())) + ")");
    }
    setVulkanInstance(&instance_);

    VkCommandPoolCreateInfo pool_info{};

    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = device_->queue_family();
    check(vkCreateCommandPool(device_->device(), &pool_info, nullptr, &pool_),
          "vkCreateCommandPool");
    for (Frame& frame : frames_) {
        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = pool_;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        try {
            check(vkAllocateCommandBuffers(device_->device(), &allocate, &frame.commands),
                  "vkAllocateCommandBuffers");
            check(vkCreateFence(device_->device(), &fence, nullptr, &frame.done), "vkCreateFence");
        } catch (...) {
            drop_presenter();
            for (Frame& made : frames_) {
                if (made.done != VK_NULL_HANDLE) {
                    vkDestroyFence(device_->device(), made.done, nullptr);
                }
            }
            vkDestroyCommandPool(device_->device(), pool_, nullptr);
            throw;
        }
    }
}

VulkanWindow::~VulkanWindow() {
    device_->wait_idle();
    drop_presenter();
    for (const Frame& frame : frames_) {
        vkDestroyFence(device_->device(), frame.done, nullptr);
    }
    vkDestroyCommandPool(device_->device(), pool_, nullptr); // frees the command buffers
    // The native window and its surface go before the instance they were made from; leaving it to
    // ~QWindow would destroy the instance (a member) first.
    destroy();
    instance_.destroy();
}

QSize VulkanWindow::pixel_size() const {
    const double ratio = devicePixelRatio();
    return {static_cast<int>(std::lround(width() * ratio)),
            static_cast<int>(std::lround(height() * ratio))};
}

std::uint32_t VulkanWindow::swapchains_created() const {
    return presenter_ ? presenter_->swapchains_created() : 0;
}

void VulkanWindow::request_frame() {
    skipped_ = 0;
    requestUpdate();
}

bool VulkanWindow::ensure_presenter() {
    if (presenter_) {
        return true;
    }
    if (surface_lost_ || !isExposed()) {
        return false;
    }
    const VkSurfaceKHR surface = QVulkanInstance::surfaceForWindow(this);
    if (surface == VK_NULL_HANDLE) {
        return false;
    }
    const QSize size = pixel_size();
    try {
        presenter_ = std::make_unique<gpu::Presenter>(*device_, surface,
                                                      static_cast<std::uint32_t>(size.width()),
                                                      static_cast<std::uint32_t>(size.height()));
    } catch (const std::exception& error) {
        std::cerr << "curation: the viewport cannot present to its window: " << error.what()
                  << '\n';
        surface_lost_ = true;
        return false;
    }
    return true;
}

void VulkanWindow::drop_presenter() noexcept {
    if (presenter_) {
        // The presenter's fences are private, and our frames may be in flight with its images.
        device_->wait_idle();
        presenter_.reset();
    }
}

bool VulkanWindow::draw(bool read_back, QImage* image) {
    ViewRenderer& renderer = *viewport_->renderer_;
    if (!renderer.ready() || !ensure_presenter()) {
        return false;
    }
    const VkDevice device = device_->device();
    Frame& frame = frames_[frame_];
    check(vkWaitForFences(device, 1, &frame.done, VK_TRUE, fence_timeout_ns), "vkWaitForFences");

    // Recorded first, so that a camera the tier refuses leaves the fence signalled.
    check(vkResetCommandBuffer(frame.commands, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(frame.commands, &begin), "vkBeginCommandBuffer");
    try {
        renderer.record(frame.commands);
    } catch (...) {
        vkEndCommandBuffer(frame.commands);
        throw;
    }
    check(vkEndCommandBuffer(frame.commands), "vkEndCommandBuffer");

    check(vkResetFences(device, 1, &frame.done), "vkResetFences");
    VkCommandBufferSubmitInfo command_info{};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    command_info.commandBuffer = frame.commands;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command_info;
    check(vkQueueSubmit2(device_->queue(), 1, &submit, frame.done), "vkQueueSubmit2");
    frame_ = (frame_ + 1) % static_cast<std::uint32_t>(frames_.size());

    const gpu::PresentSource source = renderer.present_source();
    if (!read_back) {
        return presenter_->present(source);
    }
    if (!presenter_->can_read_back()) {
        return presenter_->present(source);
    }
    const std::optional<gpu::PresentedImage> presented = presenter_->present_and_read_back(source);
    if (!presented) {
        return false;
    }
    QImage copy(reinterpret_cast<const uchar*>(presented->rgba.data()),
                static_cast<int>(presented->width), static_cast<int>(presented->height),
                static_cast<qsizetype>(presented->width) * 4, QImage::Format_RGBA8888);
    *image = copy.copy();
    image->setDevicePixelRatio(devicePixelRatio());
    return true;
}

QImage VulkanWindow::grab_frame() {
    QImage image;
    try {
        if (draw(true, &image)) {
            viewport_->frame_done();
        }
    } catch (const std::exception& error) {
        std::cerr << "curation: the viewport could not draw: " << error.what() << '\n';
        drop_presenter();
        surface_lost_ = true;
    }
    return image;
}

bool VulkanWindow::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::UpdateRequest: {
        bool shown = false;
        try {
            shown = draw(false, nullptr);
        } catch (const std::invalid_argument& error) {
            // A camera the tier refuses: nothing was submitted, and the next camera may do.
            std::cerr << "curation: the viewport could not draw: " << error.what() << '\n';
        } catch (const std::exception& error) {
            // A lost surface, or another failure after which the presenter may only be
            // destroyed: a new one is made at the next expose.
            std::cerr << "curation: the viewport could not draw: " << error.what() << '\n';
            drop_presenter();
            surface_lost_ = true;
        }
        if (shown) {
            skipped_ = 0;
            viewport_->frame_done();
        } else if (presenter_ && ++skipped_ < max_skipped_frames) {
            requestUpdate();
        }
        return true;
    }
    case QEvent::PlatformSurface:
        if (static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() ==
            QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            // The swapchain goes before its surface (ADR 0011, point 3).
            drop_presenter();
        }
        break;
    case QEvent::DevicePixelRatioChange:
        // A move to a screen of another scale: the swapchain follows the size in device pixels.
        if (presenter_) {
            const QSize size = pixel_size();
            presenter_->resize(static_cast<std::uint32_t>(size.width()),
                               static_cast<std::uint32_t>(size.height()));
        }
        request_frame();
        break;
    default:
        break;
    }
    return QWindow::event(event);
}

void VulkanWindow::exposeEvent(QExposeEvent* /*event*/) {
    if (isExposed()) {
        surface_lost_ = false;
        request_frame();
    }
}

void VulkanWindow::resizeEvent(QResizeEvent* /*event*/) {
    if (presenter_) {
        const QSize size = pixel_size();
        presenter_->resize(static_cast<std::uint32_t>(size.width()),
                           static_cast<std::uint32_t>(size.height()));
    }
    request_frame();
}

} // namespace cs::ui
