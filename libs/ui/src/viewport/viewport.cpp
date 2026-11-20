// SPDX-License-Identifier: Apache-2.0
#include "viewport/viewport.hpp"

#include "viewport/gpu_context.hpp"
#include "viewport/headless_view.hpp"
#include "viewport/pointer_lock.hpp"
#include "viewport/view_renderer.hpp"
#include "viewport/vulkan_window.hpp"

#include <QEvent>
#include <QGuiApplication>
#include <QVBoxLayout>

#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>

namespace cs::ui {

bool platform_has_vulkan_surfaces() {
    const QString platform = QGuiApplication::platformName();
    return platform != QLatin1String("offscreen") && platform != QLatin1String("minimal");
}

Viewport::Viewport(GpuContext& gpu, bool headless, QWidget* parent)
    : QWidget(parent), renderer_(std::make_unique<ViewRenderer>(gpu.device())) {
    setObjectName(QStringLiteral("viewport"));
    setMinimumSize(64, 64);
    renderer_->set_ev100(exposure_.ev100());

    settle_timer_.setSingleShot(true);
    settle_timer_.setInterval(settle_ms);
    connect(&settle_timer_, &QTimer::timeout, this, &Viewport::settle_size);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    if (headless) {
        headless_view_ = new HeadlessView(*renderer_, this);
        connect(headless_view_, &HeadlessView::frame_drawn, this, &Viewport::frame_done);
        layout->addWidget(headless_view_);
    } else {
        window_ = new VulkanWindow(gpu.device(), *renderer_);
        connect(window_, &VulkanWindow::frame_drawn, this, &Viewport::frame_done);
        container_ = QWidget::createWindowContainer(window_, this);
        layout->addWidget(container_);
    }
}

Viewport::~Viewport() {
    pointer_lock_.reset();
    // The window's frames use the renderer's images, so the window goes first: its destructor
    // waits for the device. The renderer (a member) goes after this body.
    delete container_;
    delete headless_view_;
}

std::vector<std::string> Viewport::set_world(const world::World& world,
                                             const std::filesystem::path& assets_folder) {
    std::vector<std::string> problems = renderer_->set_world(world, assets_folder);
    if (!world.cameras.empty()) {
        show_viewpoint(world.cameras.front());
    } else {
        set_camera(default_camera());
    }
    return problems;
}

void Viewport::show_viewpoint(const world::Camera& viewpoint) {
    const double before = exposure_.ev100();
    exposure_.view_from(viewpoint);
    renderer_->set_camera(viewpoint);
    exposure_changed(before);
}

void Viewport::set_camera(const world::Camera& camera) {
    renderer_->set_camera(camera);
    request_frame();
}

void Viewport::reset_exposure() {
    const double before = exposure_.ev100();
    exposure_.reset();
    exposure_changed(before);
}

void Viewport::exposure_changed(double before) {
    renderer_->set_ev100(exposure_.ev100());
    if (exposure_.ev100() != before) {
        emit ev100_changed(exposure_.ev100());
    }
    request_frame();
}

const world::Camera& Viewport::camera() const {
    return renderer_->camera();
}

QSize Viewport::render_size() const {
    return {static_cast<int>(renderer_->width()), static_cast<int>(renderer_->height())};
}

std::uint32_t Viewport::swapchains_created() const {
    return window_ != nullptr ? window_->swapchains_created() : 0;
}

QImage Viewport::grab_frame() {
    if (headless_view_ != nullptr) {
        return headless_view_->frame();
    }
    return window_->grab_frame();
}

bool Viewport::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::Resize:
    case QEvent::Show:
    case QEvent::DevicePixelRatioChange:
        size_changed();
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

void Viewport::size_changed() {
    if (renderer_->width() == 0 || renderer_->height() == 0) {
        // The first size: draw at once rather than wait for it to settle.
        settle_size();
    } else {
        settle_timer_.start();
        // The window's swapchain follows at once; the old frame is stretched meanwhile.
        request_frame();
    }
}

void Viewport::settle_size() {
    if (!isVisible()) {
        return;
    }
    const double ratio = devicePixelRatioF();
    const auto width = static_cast<std::uint32_t>(std::lround(this->width() * ratio));
    const auto height = static_cast<std::uint32_t>(std::lround(this->height() * ratio));
    if (width == renderer_->width() && height == renderer_->height()) {
        return;
    }
    try {
        renderer_->set_size(width, height);
    } catch (const std::exception& error) {
        std::cerr << "curation: the viewport cannot draw at " << width << " x " << height << ": "
                  << error.what() << '\n';
        renderer_->set_size(0, 0);
    }
    request_frame();
}

void Viewport::request_frame() {
    if (headless_view_ != nullptr) {
        headless_view_->request_frame();
    } else if (window_ != nullptr) {
        window_->request_frame();
    }
}

void Viewport::frame_done() {
    ++frames_;
    emit frame_drawn();
}

QWindow* Viewport::pointer_window() const {
    if (window_ != nullptr) {
        return window_;
    }
    // Headless, the viewport is a plain widget: its top-level window takes the pointer.
    return window()->windowHandle();
}

PointerLock& Viewport::pointer_lock() {
    if (!pointer_lock_) {
        pointer_lock_ = std::make_unique<PointerLock>(pointer_window());
        connect(pointer_lock_.get(), &PointerLock::locked, this,
                [this] { emit pointer_locked_changed(true); });
        connect(pointer_lock_.get(), &PointerLock::unlocked, this,
                [this] { emit pointer_locked_changed(false); });
        connect(pointer_lock_.get(), &PointerLock::motion, this, &Viewport::pointer_motion);
    }
    return *pointer_lock_;
}

bool Viewport::lock_pointer() {
    QWindow* window = pointer_window();
    if (window == nullptr) {
        return false;
    }
    // Esc reaches the lock only if the window has the keyboard focus: on Wayland there is no
    // keyboard grab.
    window->requestActivate();
    return pointer_lock().lock();
}

void Viewport::unlock_pointer() {
    if (pointer_lock_) {
        pointer_lock_->unlock();
    }
}

bool Viewport::pointer_locked() const {
    return pointer_lock_ && pointer_lock_->is_locked();
}

} // namespace cs::ui
