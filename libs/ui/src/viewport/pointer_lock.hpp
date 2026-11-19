// SPDX-License-Identifier: Apache-2.0
//
// Relative pointer mode for walking (ADR 0011, "Relative mouse mode"). While walk mode is on,
// the pointer stays in one window with the cursor hidden, and motion() reports how far the mouse
// moved rather than where the cursor is. Qt has no public API for this, so there are two backends:
//
// - On the wayland platform, when the compositor offers the pointer-constraints and
//   relative-pointer protocols, the window's surface locks the pointer and motion() carries the
//   compositor's unaccelerated deltas (wayland_pointer.hpp). On Wayland without them, lock()
//   fails: warping the cursor is impossible there (ADR 0011, point 9).
// - Elsewhere the window grabs the mouse, the cursor is hidden and warped back to the window's
//   centre after each move, and the warp's own move is not motion. This passed on Windows; on
//   Xwayland the warp does not hold (ADR 0011). Raw input and XInput2 are for later.
//
// Walk mode ends on unlock(), Esc, the window losing focus or its surface, and the compositor
// ending the lock. Each emits unlocked() once and shows the cursor again (ADR 0011, point 10).
//
// One PointerLock per window: on Wayland a second lock of the same surface is a protocol error,
// which disconnects the application.
#pragma once

#include <QCursor>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QWindow>

#include <memory>

namespace cs::ui {

class WaylandPointer;

class PointerLock : public QObject {
    Q_OBJECT
  public:
    explicit PointerLock(QWindow* window, QObject* parent = nullptr);
    ~PointerLock() override;

    // Starts walk mode. Returns false, changing nothing, when the window is gone or not exposed,
    // or on Wayland when the compositor lacks a protocol. True at once if already on.
    bool lock();
    // Ends walk mode, if it is on.
    void unlock();
    // Walk mode is on: from a lock() that succeeded until unlocked().
    bool is_locked() const { return walking_; }

  signals:
    // The pointer is held and motion() flows: from within lock() on the fallback, when the
    // compositor grants the lock on Wayland (which may wait for the pointer to enter the window).
    void locked();
    // Walk mode ended; the cursor is visible again.
    void unlocked();
    // Relative motion in logical pixels, x to the right and y down.
    void motion(double dx, double dy);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void fallback_move(QPointF position);
    void warp_to_centre();

    QPointer<QWindow> window_;
    std::unique_ptr<WaylandPointer> wayland_; // null off the wayland platform
    bool walking_ = false;
    QCursor previous_cursor_;
    // The fallback: where the cursor is, in logical pixels of the window (the centre after a
    // warp, then where the warp's move landed), and whether that move is still to come.
    QPointF last_position_;
    bool warp_pending_ = false;
};

} // namespace cs::ui
