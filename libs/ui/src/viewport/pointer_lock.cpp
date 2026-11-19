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
        // Direct, so that a late event cannot end a later lock. unlock() then destroys the
        // protocol objects inside their own event, which libwayland allows: it keeps the proxy
        // alive until the dispatch ends, and the handler touches nothing after emitting.
        connect(wayland_.get(), &WaylandPointer::unlocked, this, &PointerLock::unlock);
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
        // Best effort: some platforms (offscreen) have no grab, and Esc and focus loss still
        // end walk mode there.
        window_->setMouseGrabEnabled(true);
        window_->setKeyboardGrabEnabled(true);
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
    // Motion is measured from where the cursor was: after a warp, the centre. The warp's own move
    // lands there give or take the rounding to device pixels; it is not motion, but it says where
    // the cursor really is, so that the rounding does not add up into a drift. On Windows it can
    // instead merge into the user's next move, which is then measured from the centre.
    if (warp_pending_) {
        warp_pending_ = false;
        const QPointF from_centre = position - last_position_;
        const double device_pixel = 1.0 / window_->devicePixelRatio();
        if (std::abs(from_centre.x()) <= device_pixel &&
            std::abs(from_centre.y()) <= device_pixel) {
            last_position_ = position;
            return;
        }
    }
    const QPointF delta = position - last_position_;
    if (delta.isNull()) {
        return;
    }
    warp_to_centre();
    emit motion(delta.x(), delta.y());
}

void PointerLock::warp_to_centre() {
    const QPoint centre = centre_of(*window_);
    const QPoint target = window_->mapToGlobal(centre);
    // Qt skips a warp to where the cursor already is, and then no move comes back.
    warp_pending_ = QCursor::pos(window_->screen()) != target;
    QCursor::setPos(window_->screen(), target);
    last_position_ = centre;
}

} // namespace cs::ui
