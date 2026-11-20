// SPDX-License-Identifier: Apache-2.0
#include "viewport/headless_view.hpp"

#include "viewport/view_renderer.hpp"
#include "viewport/viewport.hpp"

#include <QPainter>
#include <QTimer>

#include <cstdint>
#include <exception>
#include <iostream>
#include <vector>

namespace cs::ui {

HeadlessView::HeadlessView(Viewport& viewport) : QWidget(&viewport), viewport_(&viewport) {
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void HeadlessView::request_frame() {
    if (pending_) {
        return;
    }
    pending_ = true;
    QTimer::singleShot(0, this, [this] {
        pending_ = false;
        draw();
    });
}

void HeadlessView::draw() {
    const ViewRenderer& renderer = *viewport_->renderer_;
    if (!renderer.ready()) {
        return;
    }
    try {
        const std::vector<std::uint8_t> rgba = renderer.render();
        const QImage image(rgba.data(), static_cast<int>(renderer.width()),
                           static_cast<int>(renderer.height()),
                           static_cast<qsizetype>(renderer.width()) * 4, QImage::Format_RGBA8888);
        frame_ = image.copy();
        frame_.setDevicePixelRatio(devicePixelRatioF());
    } catch (const std::exception& error) {
        std::cerr << "curation: the viewport could not draw: " << error.what() << '\n';
        return;
    }
    update();
    viewport_->frame_done();
}

void HeadlessView::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    if (frame_.isNull()) {
        painter.fillRect(rect(), Qt::black);
        return;
    }
    // Stretched while a new size settles, as the presenter stretches the window's.
    painter.drawImage(rect(), frame_);
}

} // namespace cs::ui
