// SPDX-License-Identifier: Apache-2.0
// Relative mouse mode on Wayland. Qt has no public API for it and Wayland forbids warping the
// cursor, so walking needs two protocols bound by us: zwp_pointer_constraints_v1 (lock the
// pointer to the viewport's surface) and zwp_relative_pointer_v1 (unaccelerated deltas).
#pragma once

#include <QObject>
#include <QString>
#include <memory>

class QWindow;

namespace spike {

class WaylandPointer : public QObject {
    Q_OBJECT
  public:
    explicit WaylandPointer(QObject* parent = nullptr);
    ~WaylandPointer() override;

    // True on the wayland platform when the compositor offers both protocols.
    bool available() const;
    // Locks the pointer to `window`'s surface; the compositor confirms with locked().
    bool lock(QWindow* window);
    void unlock();
    bool is_locked() const { return locked_; }
    QString describe() const;

  signals:
    void motion(double dx, double dy); // unaccelerated, in surface-local logical pixels
    void locked();
    void unlocked();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool locked_ = false;
};

} // namespace spike
