// SPDX-License-Identifier: Apache-2.0
#include "wayland_pointer.hpp"

#include <QGuiApplication>
#include <QWindow>

#if SPIKE_HAS_WAYLAND
#include <QtWaylandClient/QWaylandClientExtension>
#include <qpa/qplatformnativeinterface.h>

#include "qwayland-pointer-constraints-unstable-v1.h"
#include "qwayland-relative-pointer-unstable-v1.h"
#endif

namespace spike {

#if SPIKE_HAS_WAYLAND

namespace {

class Constraints : public QWaylandClientExtensionTemplate<Constraints>,
                    public QtWayland::zwp_pointer_constraints_v1 {
  public:
    Constraints() : QWaylandClientExtensionTemplate(1) { initialize(); }
};

class RelativeManager : public QWaylandClientExtensionTemplate<RelativeManager>,
                        public QtWayland::zwp_relative_pointer_manager_v1 {
  public:
    RelativeManager() : QWaylandClientExtensionTemplate(1) { initialize(); }
};

class LockedPointer : public QtWayland::zwp_locked_pointer_v1 {
  public:
    LockedPointer(::zwp_locked_pointer_v1* object, WaylandPointer* owner)
        : QtWayland::zwp_locked_pointer_v1(object), owner_(owner) {}
    ~LockedPointer() override { destroy(); }

  protected:
    void zwp_locked_pointer_v1_locked() override { QMetaObject::invokeMethod(owner_, "locked"); }
    void zwp_locked_pointer_v1_unlocked() override {
        QMetaObject::invokeMethod(owner_, "unlocked");
    }

  private:
    WaylandPointer* owner_;
};

class RelativePointer : public QtWayland::zwp_relative_pointer_v1 {
  public:
    RelativePointer(::zwp_relative_pointer_v1* object, WaylandPointer* owner)
        : QtWayland::zwp_relative_pointer_v1(object), owner_(owner) {}
    ~RelativePointer() override { destroy(); }

  protected:
    void zwp_relative_pointer_v1_relative_motion(uint32_t, uint32_t, wl_fixed_t, wl_fixed_t,
                                                 wl_fixed_t dx_unaccel,
                                                 wl_fixed_t dy_unaccel) override {
        emit owner_->motion(wl_fixed_to_double(dx_unaccel), wl_fixed_to_double(dy_unaccel));
    }

  private:
    WaylandPointer* owner_;
};

} // namespace

struct WaylandPointer::Impl {
    std::unique_ptr<Constraints> constraints;
    std::unique_ptr<RelativeManager> relative_manager;
    std::unique_ptr<LockedPointer> locked;
    std::unique_ptr<RelativePointer> relative;
};

WaylandPointer::WaylandPointer(QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>()) {
    if (QGuiApplication::platformName() == QLatin1String("wayland")) {
        impl_->constraints = std::make_unique<Constraints>();
        impl_->relative_manager = std::make_unique<RelativeManager>();
    }
    connect(this, &WaylandPointer::locked, this, [this] { locked_ = true; });
    connect(this, &WaylandPointer::unlocked, this, [this] { locked_ = false; });
}

WaylandPointer::~WaylandPointer() {
    unlock();
}

bool WaylandPointer::available() const {
    return impl_->constraints && impl_->constraints->isActive() && impl_->relative_manager &&
           impl_->relative_manager->isActive();
}

bool WaylandPointer::lock(QWindow* window) {
    if (!available()) {
        return false;
    }
    auto* app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    // The wl_surface of a window is only reachable through the QPA native interface (a Qt
    // private header): this is the one private-API use the prototype needs.
    auto* surface = static_cast<::wl_surface*>(
        QGuiApplication::platformNativeInterface()->nativeResourceForWindow("surface", window));
    if (!app || !app->pointer() || !surface) {
        return false;
    }
    impl_->locked = std::make_unique<LockedPointer>(
        impl_->constraints->lock_pointer(
            surface, app->pointer(), nullptr,
            QtWayland::zwp_pointer_constraints_v1::lifetime_persistent),
        this);
    impl_->relative = std::make_unique<RelativePointer>(
        impl_->relative_manager->get_relative_pointer(app->pointer()), this);
    return true;
}

void WaylandPointer::unlock() {
    impl_->relative.reset();
    impl_->locked.reset();
    locked_ = false;
}

QString WaylandPointer::describe() const {
    if (!available()) {
        return QStringLiteral("protocols missing");
    }
    if (!impl_->locked) {
        return QStringLiteral("free");
    }
    return locked_ ? QStringLiteral("locked by compositor") : QStringLiteral("lock pending");
}

#else // no Qt Wayland client module: the generic path is used everywhere

struct WaylandPointer::Impl {};
WaylandPointer::WaylandPointer(QObject* parent) : QObject(parent) {}
WaylandPointer::~WaylandPointer() = default;
bool WaylandPointer::available() const {
    return false;
}
bool WaylandPointer::lock(QWindow*) {
    return false;
}
void WaylandPointer::unlock() {}
QString WaylandPointer::describe() const {
    return QStringLiteral("not built");
}

#endif

} // namespace spike
