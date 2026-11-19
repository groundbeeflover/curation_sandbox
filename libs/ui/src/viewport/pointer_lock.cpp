// SPDX-License-Identifier: Apache-2.0
#include "viewport/pointer_lock.hpp"

#include "viewport/wayland_pointer.hpp"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>

#include <cmath>

namespace cs::ui {

namespace {

// Where the fallback warps the cursor to: whole logical pixels, as QCursor::setPos takes them.
QPoint centre_of(const QWindow& window) {
    return {window.width() / 2, window.height() / 2};
}

} // namespace

PointerLock::PointerLock(QWindow* window, QObject* parent) : QObject(parent), window_(window) {
    if (QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) {
        wayland_ = std::make_unique<WaylandPointer>();
        connect(wayland_.get(), &WaylandPointer::locked, this, &PointerLock::locked);
        connect(wayland_.get(), &WaylandPointer::motion, this, &PointerLock::motion);
        // Queued, so that the protocol objects are not destroyed inside their own event.
        connect(wayland_.get(), &WaylandPointer::unlocked, this, &PointerLock::unlock,
                Qt::QueuedConnection);
    }
}

PointerLock::~PointerLock() {
    unlock();
}

bool PointerLock::lock() {
    if (walking_) {
        return true;
    }
    if (!window_ || !window_->isExposed()) {
        return false;
    }
    if (wayland_) {
        if (!wayland_->lock(*window_)) {
            return false;
        }
    } else {
        window_->setMouseGrabEnabled(true);
        window_->setKeyboardGrabEnabled(true);
        last_position_ = window_->mapFromGlobal(QPointF(QCursor::pos(window_->screen())));
        warp_to_centre();
    }
    walking_ = true;
    previous_cursor_ = window_->cursor();
    window_->setCursor(Qt::BlankCursor);
    window_->installEventFilter(this);
    if (!wayland_) {
        emit locked();
    }
    return true;
}

void PointerLock::unlock() {
    if (!walking_) {
        return;
    }
    walking_ = false;
    warp_pending_ = false;
    if (wayland_) {
        wayland_->unlock();
    }
    if (window_) {
        window_->removeEventFilter(this);
        if (!wayland_) {
            window_->setMouseGrabEnabled(false);
            window_->setKeyboardGrabEnabled(false);
        }
        window_->setCursor(previous_cursor_);
    }
    emit unlocked();
}

bool PointerLock::eventFilter(QObject* watched, QEvent* event) {
    if (watched != window_ || !walking_) {
        return false;
    }
    switch (event->type()) {
    case QEvent::KeyPress:
        if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            unlock();
            return true;
        }
        break;
    case QEvent::FocusOut:
        unlock();
        break;
    case QEvent::PlatformSurface:
        // The surface the Wayland lock holds is going away (the window is hidden or reparented).
        if (static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() ==
            QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            unlock();
        }
        break;
    case QEvent::MouseMove:
        if (!wayland_) {
            fallback_move(static_cast<QMouseEvent*>(event)->position());
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

void PointerLock::fallback_move(QPointF position) {
    // A warp lands on the centre give or take the rounding to device pixels. That move tells
    // where the cursor now is, and is not motion; measuring the next move from where it really
    // landed keeps the rounding from adding up into a drift.
    const QPointF from_centre = position - QPointF(centre_of(*window_));
    const double device_pixel = 1.0 / window_->devicePixelRatio();
    if (warp_pending_ && std::abs(from_centre.x()) <= device_pixel &&
        std::abs(from_centre.y()) <= device_pixel) {
        warp_pending_ = false;
        last_position_ = position;
        return;
    }
    const QPointF delta = position - last_position_;
    last_position_ = position;
    if (delta.isNull()) {
        return;
    }
    warp_to_centre();
    emit motion(delta.x(), delta.y());
}

void PointerLock::warp_to_centre() {
    QCursor::setPos(window_->screen(), window_->mapToGlobal(centre_of(*window_)));
    warp_pending_ = true;
}

} // namespace cs::ui
