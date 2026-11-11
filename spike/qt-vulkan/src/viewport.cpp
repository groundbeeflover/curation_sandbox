// SPDX-License-Identifier: Apache-2.0
#include "viewport.hpp"

#include "wayland_pointer.hpp"

#include <QCursor>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPlatformSurfaceEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace spike {

namespace {
constexpr double spin_rad_per_s = 0.8;
constexpr double look_rad_per_px = 0.004;
} // namespace

FrameParams ViewportState::params() const {
    FrameParams p;
    const double t = clock.isValid() ? clock.elapsed() * 1e-3 : 0.0;
    p.angle_rad = float(std::fmod(t * spin_rad_per_s + yaw_rad, 2.0 * M_PI));
    p.offset_y = float(pitch);
    return p;
}

void ViewportState::add_relative(double dx, double dy) {
    relative_dx_total += dx;
    relative_dy_total += dy;
    ++relative_events;
    yaw_rad += dx * look_rad_per_px;
    pitch = std::clamp(pitch + dy * 0.002, -0.5, 0.5);
}

QImage ViewportState::readback_image() const {
    const Readback& r = renderer.readback();
    if (r.rgba.empty()) {
        return {};
    }
    return QImage(r.rgba.data(), int(r.width), int(r.height), int(r.width) * 4,
                  QImage::Format_RGBA8888)
        .copy();
}

// ---- VulkanViewportWindow ---------------------------------------------------------------------

VulkanViewportWindow::VulkanViewportWindow(bool validation) {
    setSurfaceType(QSurface::VulkanSurface);
    state_.clock.start();
    if (!state_.renderer.create_instance(validation)) {
        state_.init_error = QString::fromStdString(state_.renderer.last_error());
        return;
    }
    // Qt does not create or own the instance: it wraps ours to make the window's surface.
    qvk_.setVkInstance(state_.renderer.instance());
    if (!qvk_.create()) {
        state_.init_error = QStringLiteral("QVulkanInstance::create failed with our VkInstance");
        return;
    }
    setVulkanInstance(&qvk_);
    wayland_pointer_ = new WaylandPointer(this);
    connect(wayland_pointer_, &WaylandPointer::motion, this,
            [this](double dx, double dy) { state_.add_relative(dx, dy); });
    connect(wayland_pointer_, &WaylandPointer::locked, this, [this] {
        state_.pointer_mode = QStringLiteral("wayland: locked by compositor");
        std::fprintf(stderr, "[viewport] pointer locked by the compositor\n");
    });
    connect(wayland_pointer_, &WaylandPointer::unlocked, this, [this] {
        state_.pointer_mode = QStringLiteral("wayland: unlocked");
        std::fprintf(stderr, "[viewport] pointer unlocked\n");
    });
}

VulkanViewportWindow::~VulkanViewportWindow() {
    // The surface belongs to Qt and must go before our instance; the swapchain before the
    // surface (handled by the SurfaceAboutToBeDestroyed event that destroy() sends).
    destroy();
    qvk_.destroy();
    state_.renderer.destroy();
}

void VulkanViewportWindow::initialise() {
    initialised_ = true;
    if (!state_.init_error.isEmpty()) {
        return;
    }
    const VkSurfaceKHR surface = QVulkanInstance::surfaceForWindow(this);
    if (!surface) {
        state_.init_error = QStringLiteral("QVulkanInstance::surfaceForWindow returned null");
        return;
    }
    if (state_.renderer.has_device()) {
        state_.renderer.set_surface(surface);
    } else if (!state_.renderer.create_device(surface)) {
        state_.init_error = QString::fromStdString(state_.renderer.last_error());
    }
}

void VulkanViewportWindow::render(bool capture) {
    if (!initialised_ || !state_.init_error.isEmpty() || !isExposed()) {
        return;
    }
    const qreal dpr = devicePixelRatio();
    state_.renderer.resize(uint32_t(std::lround(width() * dpr)),
                           uint32_t(std::lround(height() * dpr)));
    if (!state_.renderer.draw_frame(state_.params(), capture)) {
        state_.init_error = QString::fromStdString(state_.renderer.last_error());
    }
}

QImage VulkanViewportWindow::capture() {
    render(true);
    return state_.readback_image();
}

void VulkanViewportWindow::exposeEvent(QExposeEvent*) {
    if (isExposed()) {
        if (!initialised_) {
            initialise();
        }
        render(false);
        requestUpdate();
    }
}

bool VulkanViewportWindow::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::UpdateRequest:
        render(false);
        requestUpdate(); // continuous animation, paced by the swapchain's FIFO present
        return true;
    case QEvent::PlatformSurface:
        if (static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() ==
            QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            // Reparenting or hiding can destroy the native window; the next expose makes a new
            // surface and the same device presents to it.
            state_.renderer.release_swapchain();
            initialised_ = false;
        }
        break;
    default:
        break;
    }
    return QWindow::event(event);
}

QPoint VulkanViewportWindow::centre_global() const {
    return mapToGlobal(QPoint(width() / 2, height() / 2));
}

void VulkanViewportWindow::set_walk(bool on) {
    if (on == state_.walking) {
        return;
    }
    state_.walking = on;
    if (on) {
        requestActivate();
        setCursor(Qt::BlankCursor);
        if (wayland_pointer_ && wayland_pointer_->available()) {
            // Wayland: the cursor cannot be warped; lock it and read relative motion.
            wayland_pointer_->lock(this);
            state_.pointer_mode = QStringLiteral("wayland: lock pending");
        } else {
            // X11 and Windows: grab, hide, and warp back to the centre after every move.
            setMouseGrabEnabled(true);
            setKeyboardGrabEnabled(true);
            warping_ = true;
            QCursor::setPos(screen(), centre_global());
            const QPoint now = QCursor::pos(screen());
            std::fprintf(stderr, "[viewport] warp to %d,%d -> cursor at %d,%d\n",
                         centre_global().x(), centre_global().y(), now.x(), now.y());
            state_.pointer_mode = QStringLiteral("grab + warp to centre");
        }
    } else {
        if (wayland_pointer_) {
            wayland_pointer_->unlock();
        }
        setMouseGrabEnabled(false);
        setKeyboardGrabEnabled(false);
        warping_ = false;
        unsetCursor();
        state_.pointer_mode = QStringLiteral("free");
    }
    std::fprintf(stderr, "[viewport] walk %s (%s)\n", on ? "on" : "off",
                 qPrintable(state_.pointer_mode));
    emit walk_changed(on);
}

void VulkanViewportWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton) {
        set_walk(!state_.walking);
    }
}

void VulkanViewportWindow::mouseMoveEvent(QMouseEvent* event) {
    if (!state_.walking || !warping_) {
        return;
    }
    const QPointF centre = mapFromGlobal(centre_global());
    const QPointF delta = event->position() - centre;
    // The warp itself produces a motion event at (or within rounding of) the centre.
    if (std::abs(delta.x()) < 0.75 && std::abs(delta.y()) < 0.75) {
        return;
    }
    if (state_.relative_events < 8) {
        std::fprintf(stderr, "[viewport] move at %.2f,%.2f (global %.2f,%.2f), centre %.2f,%.2f\n",
                     event->position().x(), event->position().y(), event->globalPosition().x(),
                     event->globalPosition().y(), centre.x(), centre.y());
    }
    state_.add_relative(delta.x(), delta.y());
    QCursor::setPos(screen(), centre_global());
}

void VulkanViewportWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && state_.walking) {
        set_walk(false);
    }
}

void VulkanViewportWindow::focusOutEvent(QFocusEvent*) {
    if (state_.walking && !(wayland_pointer_ && wayland_pointer_->is_locked())) {
        set_walk(false);
    }
}

// ---- HeadlessViewportWidget -------------------------------------------------------------------

HeadlessViewportWidget::HeadlessViewportWidget(bool validation, QWidget* parent) : QWidget(parent) {
    state_.clock.start();
    ok_ = state_.renderer.create_instance(validation) &&
          state_.renderer.create_device(VK_NULL_HANDLE);
    if (!ok_) {
        state_.init_error = QString::fromStdString(state_.renderer.last_error());
    }
    state_.pointer_mode = QStringLiteral("headless");
    startTimer(16);
}

void HeadlessViewportWidget::render() {
    if (!ok_ || width() <= 0 || height() <= 0) {
        return;
    }
    const qreal dpr = devicePixelRatioF();
    state_.renderer.resize(uint32_t(std::lround(width() * dpr)),
                           uint32_t(std::lround(height() * dpr)));
    if (state_.renderer.draw_frame(state_.params(), true)) {
        frame_ = state_.readback_image();
        frame_.setDevicePixelRatio(dpr);
        update();
    }
}

void HeadlessViewportWidget::timerEvent(QTimerEvent*) {
    render();
}

void HeadlessViewportWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    if (frame_.isNull()) {
        p.fillRect(rect(), Qt::black);
        p.setPen(Qt::white);
        p.drawText(rect(), Qt::AlignCenter,
                   state_.init_error.isEmpty() ? QStringLiteral("no frame yet")
                                               : state_.init_error);
        return;
    }
    p.drawImage(rect(), frame_);
}

} // namespace spike
