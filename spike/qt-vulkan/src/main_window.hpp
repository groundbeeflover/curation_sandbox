// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QImage>
#include <QMainWindow>

class QLabel;
class QPushButton;

namespace spike {

class VulkanViewportWindow;
class HeadlessViewportWidget;
struct ViewportState;

class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    // `headless` is forced on the offscreen and minimal platforms, which have no surfaces.
    MainWindow(bool headless, bool validation);

    ViewportState& viewport_state();
    QWidget* viewport_widget() const { return viewport_widget_; }
    VulkanViewportWindow* viewport_window() const { return window_; }

    // The widgets as QWidget::grab sees them, with the viewport's last frame painted where
    // its native window sits (grab cannot see a native child window).
    QImage grab_composite();
    QString status_line();

  private:
    void refresh();

    VulkanViewportWindow* window_ = nullptr;
    HeadlessViewportWidget* headless_ = nullptr;
    QWidget* viewport_widget_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* walk_button_ = nullptr;
};

} // namespace spike
