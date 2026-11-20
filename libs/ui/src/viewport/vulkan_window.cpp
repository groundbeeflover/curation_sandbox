// SPDX-License-Identifier: Apache-2.0
#include "viewport/vulkan_window.hpp"

#include "viewport/view_renderer.hpp"

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

VulkanWindow::VulkanWindow(const gpu::Device& device, ViewRenderer& renderer)
    : device_(&device), renderer_(&renderer) {
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

void VulkanWindow::resize_presenter() {
    if (presenter_) {
        const QSize size = pixel_size();
        presenter_->resize(static_cast<std::uint32_t>(size.width()),
                           static_cast<std::uint32_t>(size.height()));
    }
}

void VulkanWindow::drop_presenter() noexcept {
    if (presenter_) {
        // The presenter's fences are private, and our frames may be in flight with its images.
        device_->wait_idle();
        presenter_.reset();
    }
}

bool VulkanWindow::draw(QImage* read_back) {
    ViewRenderer& renderer = *renderer_;
    // A hidden window presents nothing: under Wayland's FIFO mode each present would wait.
    if (!isExposed() || !renderer.ready() || !ensure_presenter()) {
        return false;
    }
    if (renderer.stale()) {
        const VkDevice device = device_->device();
        Frame& frame = frames_[frame_];
        check(vkWaitForFences(device, 1, &frame.done, VK_TRUE, fence_timeout_ns),
              "vkWaitForFences");

        // Recorded first, so that a camera the tier refuses leaves the fence signalled.
        check(vkResetCommandBuffer(frame.commands, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(frame.commands, &begin), "vkBeginCommandBuffer");
        try {
            renderer.record(frame.commands);
        } catch (...) {
            // Recording may have stopped inside a rendering instance, where ending is invalid;
            // the pool lets a command buffer be reset on its own.
            vkResetCommandBuffer(frame.commands, 0);
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
        const VkResult submitted = vkQueueSubmit2(device_->queue(), 1, &submit, frame.done);
        if (submitted != VK_SUCCESS) {
            // The fence was reset and nothing will signal it: make it again, signalled, so that
            // the next frame does not wait a minute for it.
            vkDestroyFence(device, frame.done, nullptr);
            frame.done = VK_NULL_HANDLE;
            VkFenceCreateInfo fence{};
            fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            check(vkCreateFence(device, &fence, nullptr, &frame.done), "vkCreateFence");
            check(submitted, "vkQueueSubmit2");
        }
        renderer.mark_recorded();
        frame_ = (frame_ + 1) % static_cast<std::uint32_t>(frames_.size());
    }

    const gpu::PresentSource source = renderer.present_source();
    if (read_back == nullptr || !presenter_->can_read_back()) {
        return presenter_->present(source);
    }
    const std::optional<gpu::PresentedImage> presented = presenter_->present_and_read_back(source);
    if (!presented) {
        return false;
    }
    const QImage view(reinterpret_cast<const uchar*>(presented->rgba.data()),
                      static_cast<int>(presented->width), static_cast<int>(presented->height),
                      static_cast<qsizetype>(presented->width) * 4, QImage::Format_RGBA8888);
    *read_back = view.copy();
    read_back->setDevicePixelRatio(devicePixelRatio());
    return true;
}

bool VulkanWindow::try_draw(QImage* read_back) {
    try {
        if (draw(read_back)) {
            emit frame_drawn();
            return true;
        }
    } catch (const std::invalid_argument& error) {
        // A camera the tier refuses: the frame was not recorded, and the next camera may do.
        std::cerr << "curation: the viewport could not draw: " << error.what() << '\n';
    } catch (const std::exception& error) {
        // A lost surface, or another failure after which the presenter may only be destroyed:
        // a new one is made at the next expose.
        std::cerr << "curation: the viewport could not draw: " << error.what() << '\n';
        drop_presenter();
        surface_lost_ = true;
    }
    return false;
}

QImage VulkanWindow::grab_frame() {
    QImage image;
    try_draw(&image);
    return image;
}

bool VulkanWindow::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::UpdateRequest:
        if (try_draw(nullptr)) {
            skipped_ = 0;
        } else if (presenter_ && isExposed() && ++skipped_ < max_skipped_frames) {
            requestUpdate();
        }
        return true;
    case QEvent::PlatformSurface:
        if (static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() ==
            QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            // The swapchain goes before its surface (ADR 0011, point 3).
            drop_presenter();
        }
        break;
    case QEvent::DevicePixelRatioChange:
        // A move to a screen of another scale: the swapchain follows the size in device pixels.
        resize_presenter();
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
    resize_presenter();
    request_frame();
}

} // namespace cs::ui
