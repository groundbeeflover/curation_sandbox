// SPDX-License-Identifier: Apache-2.0
// The adapter between Qt and the Qt-free renderer: what libs/ui's viewport will become.
#pragma once

#include "vk_renderer.hpp"

#include <QElapsedTimer>
#include <QImage>
#include <QPointF>
#include <QVulkanInstance>
#include <QWidget>
#include <QWindow>

namespace spike {

class WaylandPointer;

// What both viewport variants share: the renderer, the animation and the walk state.
struct ViewportState {
    VkRenderer renderer;
    QElapsedTimer clock;
    double yaw_rad = 0.0;
    double pitch = 0.0;
    double relative_dx_total = 0.0; // relative mouse motion received in walk mode
    double relative_dy_total = 0.0;
    long relative_events = 0;
    bool walking = false;
    QString pointer_mode = QStringLiteral("free");
    QString init_error;

    FrameParams params() const;
    void add_relative(double dx, double dy);
    QImage readback_image() const; // the last captured frame, or a null image
};

// A native window with a Vulkan surface, embedded with QWidget::createWindowContainer. Our
// renderer owns the instance, device and swapchain; Qt only wraps our VkInstance to make the
// VkSurfaceKHR (QVulkanInstance::setVkInstance), as ADR 0006 proposes.
class VulkanViewportWindow : public QWindow {
    Q_OBJECT
  public:
    explicit VulkanViewportWindow(bool validation);
    ~VulkanViewportWindow() override;

    ViewportState& state() { return state_; }
    void set_walk(bool on);
    // Draws one frame now and copies it back; returns the frame or a null image.
    QImage capture();

  signals:
    void walk_changed(bool on);

  protected:
    void exposeEvent(QExposeEvent* event) override;
    bool event(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

  private:
    void initialise();
    void render(bool capture);
    QPoint centre_global() const;

    ViewportState state_;
    QVulkanInstance qvk_;
    WaylandPointer* wayland_pointer_ = nullptr;
    bool initialised_ = false;
    bool warping_ = false; // generic path: cursor warped to the centre each move
};

// The offscreen platform has no window surface: the same renderer draws into an offscreen
// image which an ordinary widget paints, so QWidget::grab shows the viewport in UI tests.
class HeadlessViewportWidget : public QWidget {
    Q_OBJECT
  public:
    explicit HeadlessViewportWidget(bool validation, QWidget* parent = nullptr);
    ViewportState& state() { return state_; }
    void render();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void timerEvent(QTimerEvent* event) override;

  private:
    ViewportState state_;
    QImage frame_;
    bool ok_ = false;
};

} // namespace spike
