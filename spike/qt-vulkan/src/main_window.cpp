// SPDX-License-Identifier: Apache-2.0
#include "main_window.hpp"

#include "viewport.hpp"

#include <QDockWidget>
#include <QGuiApplication>
#include <QLabel>
#include <QMenuBar>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

namespace spike {

MainWindow::MainWindow(bool headless, bool validation) {
    setWindowTitle(QStringLiteral("Curation Sandbox - Qt Vulkan spike"));
    resize(1100, 700);

    if (headless) {
        headless_ = new HeadlessViewportWidget(validation);
        viewport_widget_ = headless_;
    } else {
        window_ = new VulkanViewportWindow(validation);
        viewport_widget_ = QWidget::createWindowContainer(window_);
        viewport_widget_->setFocusPolicy(Qt::StrongFocus);
    }
    viewport_widget_->setMinimumSize(160, 120);
    setCentralWidget(viewport_widget_);

    QMenu* file = menuBar()->addMenu(QStringLiteral("&File"));
    file->addAction(QStringLiteral("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    auto* dock = new QDockWidget(QStringLiteral("Inspector"), this);
    dock->setObjectName(QStringLiteral("inspector"));
    auto* panel = new QWidget(dock);
    auto* layout = new QVBoxLayout(panel);
    status_ = new QLabel(panel);
    status_->setObjectName(QStringLiteral("status"));
    status_->setTextFormat(Qt::PlainText);
    status_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    status_->setMinimumWidth(260);
    walk_button_ = new QPushButton(QStringLiteral("Walk (right-click; Esc leaves)"), panel);
    walk_button_->setEnabled(window_ != nullptr);
    layout->addWidget(walk_button_);
    layout->addWidget(status_, 1);
    dock->setWidget(panel);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    menuBar()->addMenu(QStringLiteral("&View"))->addAction(dock->toggleViewAction());

    if (window_) {
        connect(walk_button_, &QPushButton::clicked, this, [this] { window_->set_walk(true); });
    }
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &MainWindow::refresh);
    timer->start(250);
    statusBar()->showMessage(QStringLiteral("platform: %1").arg(QGuiApplication::platformName()));
}

ViewportState& MainWindow::viewport_state() {
    return window_ ? window_->state() : headless_->state();
}

QString MainWindow::status_line() {
    ViewportState& s = viewport_state();
    const VkRenderer& r = s.renderer;
    const qreal dpr = viewport_widget_->devicePixelRatioF();
    QScreen* screen = viewport_widget_->screen();
    return QStringLiteral("platform=%1 device=\"%2\" screen_scale=%3 dpr=%4 viewport=%5x%6 "
                          "extent=%7x%8 swapchains=%9 frames=%10 validation=%11 pointer=\"%12\" "
                          "relative_events=%13 relative_total=%14,%15 error=\"%16\"")
        .arg(QGuiApplication::platformName(), QString::fromStdString(r.device_name()))
        .arg(screen ? screen->devicePixelRatio() : 0.0)
        .arg(dpr)
        .arg(viewport_widget_->width())
        .arg(viewport_widget_->height())
        .arg(r.extent().width)
        .arg(r.extent().height)
        .arg(r.swapchain_generation())
        .arg(r.frames())
        .arg(r.validation_messages())
        .arg(s.pointer_mode)
        .arg(s.relative_events)
        .arg(s.relative_dx_total, 0, 'f', 1)
        .arg(s.relative_dy_total, 0, 'f', 1)
        .arg(s.init_error);
}

void MainWindow::refresh() {
    ViewportState& s = viewport_state();
    const VkRenderer& r = s.renderer;
    const qreal dpr = viewport_widget_->devicePixelRatioF();
    status_->setText(
        QStringLiteral("Platform: %1\nDevice: %2\nDevice pixel ratio: %3\n"
                       "Viewport: %4 x %5 logical\nSwapchain: %6 x %7 px\n"
                       "Swapchains created: %8\nFrames: %9\nValidation messages: %10\n\n"
                       "Walk: %11\nPointer: %12\nRelative events: %13\nRelative total: %14, "
                       "%15 px\n%16")
            .arg(QGuiApplication::platformName(), QString::fromStdString(r.device_name()))
            .arg(dpr)
            .arg(viewport_widget_->width())
            .arg(viewport_widget_->height())
            .arg(r.extent().width)
            .arg(r.extent().height)
            .arg(r.swapchain_generation())
            .arg(r.frames())
            .arg(r.validation_messages())
            .arg(s.walking ? QStringLiteral("on") : QStringLiteral("off"))
            .arg(s.pointer_mode)
            .arg(s.relative_events)
            .arg(s.relative_dx_total, 0, 'f', 1)
            .arg(s.relative_dy_total, 0, 'f', 1)
            .arg(s.init_error));
}

QImage MainWindow::grab_composite() {
    refresh();
    QImage shot = grab().toImage();
    QImage frame;
    if (window_) {
        frame = window_->capture();
    } else {
        headless_->render();
        frame = viewport_state().readback_image();
    }
    if (!frame.isNull() && window_) {
        QPainter p(&shot);
        const QPoint at = viewport_widget_->mapTo(this, QPoint(0, 0));
        p.drawImage(QRectF(at, viewport_widget_->size()), frame);
    }
    return shot;
}

} // namespace spike
