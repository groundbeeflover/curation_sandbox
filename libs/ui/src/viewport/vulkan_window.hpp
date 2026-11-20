// SPDX-License-Identifier: Apache-2.0
//
// The viewport's window on a platform with window surfaces (ADR 0011): a QWindow of type
// VulkanSurface, embedded in the viewport with QWidget::createWindowContainer. It owns a
// QVulkanInstance that wraps the device's own VkInstance, takes the surface Qt makes for it, and
// presents the viewport's frames through cs::gpu::Presenter.
//
// Each frame: the renderer's StandardTier::record and ToneMapPass::record into a command buffer
// of this window, submitted, then Presenter::present() of the display image (no barriers are
// needed between them). Two frames are in flight, as in the presenter.
//
// The swapchain follows the window's size in device pixels (the logical size times
// devicePixelRatio()), on a resize and on a move to a screen of another scale. When the platform
// destroys the surface (SurfaceAboutToBeDestroyed, on reparenting) or Vulkan reports it lost, the
// presenter goes, and a new one is made on the new surface at the next expose. A window that is
// not exposed presents nothing, and three frames skipped in a row stop the requests until the
// next expose or resize, so that a hidden window under Wayland's FIFO mode cannot stall the
// interface.
#pragma once

#include <cs/gpu/device.hpp>
#include <cs/gpu/presenter.hpp>

#include <QImage>
#include <QVulkanInstance>
#include <QWindow>

#include <array>
#include <memory>

namespace cs::ui {

class Viewport;

class VulkanWindow : public QWindow {
    Q_OBJECT
  public:
    // The viewport owns the renderer and the GPU, which outlive the window. Throws
    // std::runtime_error when Qt cannot wrap the device's instance.
    explicit VulkanWindow(Viewport& viewport);
    // In this order (ADR 0011, point 2): wait for the device, free the frames and the presenter,
    // destroy the native window and its surface, then the QVulkanInstance.
    ~VulkanWindow() override;

    VulkanWindow(const VulkanWindow&) = delete;
    VulkanWindow& operator=(const VulkanWindow&) = delete;
    VulkanWindow(VulkanWindow&&) = delete;
    VulkanWindow& operator=(VulkanWindow&&) = delete;

    // Draws a frame at the next update request.
    void request_frame();
    // Presents a frame now and copies it back from the swapchain; a null image when no frame
    // could be shown or the swapchain cannot be read back.
    [[nodiscard]] QImage grab_frame();
    // How many swapchains have been made, for logs.
    [[nodiscard]] std::uint32_t swapchains_created() const;

  protected:
    bool event(QEvent* event) override;
    void exposeEvent(QExposeEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    struct Frame {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkFence done = VK_NULL_HANDLE; // created signalled
    };

    Viewport* viewport_;
    const gpu::Device* device_;
    QVulkanInstance instance_;
    std::unique_ptr<gpu::Presenter> presenter_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::array<Frame, gpu::Presenter::frames_in_flight> frames_{};
    std::uint32_t frame_ = 0;
    int skipped_ = 0; // frames in a row that present() did not show
    bool surface_lost_ =
        false; // a failure the presenter cannot recover from, until the next expose

    QSize pixel_size() const;
    bool ensure_presenter();
    void drop_presenter() noexcept;
    // Records and submits the renderer's frame, then presents. Returns what present() did.
    bool draw(bool read_back, QImage* image);
};

} // namespace cs::ui
