// SPDX-License-Identifier: Apache-2.0
//
// PointerLock's Wayland backend (ADR 0011): zwp_pointer_constraints_v1 locks the pointer to a
// window's surface, and zwp_relative_pointer_v1 reports the pointer's unaccelerated motion.
// wayland_pointer.cpp is the one file that uses Qt's Wayland client and a Qt private header;
// where they are not built in (CS_WAYLAND=0, every platform but Linux), lock() always fails.
#pragma once

#include <QObject>

#include <memory>

class QWindow;

namespace cs::ui {

class WaylandPointer : public QObject {
    Q_OBJECT
  public:
    // Binds the two protocols if the compositor offers them. Only on the wayland platform.
    WaylandPointer();
    ~WaylandPointer() override;

    // Asks the compositor to lock the pointer to the window's surface; locked() says when it
    // does. False when a protocol is missing or the window has no surface yet.
    bool lock(QWindow& window);
    // Drops the lock, if any, and stops motion. Emits nothing.
    void unlock();

  signals:
    void locked();
    // The compositor ended the lock (focus moved away, say). The lock is then defunct, and
    // unlock() should follow.
    void unlocked();
    // While locked: the unaccelerated motion in surface coordinates, which are Qt's logical
    // pixels unless QT_SCALE_FACTOR scales Qt's on top of the compositor's.
    void motion(double dx, double dy);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cs::ui
