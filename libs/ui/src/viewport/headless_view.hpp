// SPDX-License-Identifier: Apache-2.0
//
// The viewport on the offscreen platform, which has no Vulkan surface (ADR 0011, point 4): a plain
// widget that paints the display image read back from the GPU (ToneMapPass::render), so that UI
// tests run headless and QWidget::grab shows the viewport as it is.
#pragma once

#include <QImage>
#include <QWidget>

namespace cs::ui {

class ViewRenderer;

class HeadlessView : public QWidget {
    Q_OBJECT
  public:
    // The renderer must outlive the view.
    HeadlessView(const ViewRenderer& renderer, QWidget* parent);

    // Renders a frame at the next turn of the event loop.
    void request_frame();
    // The last frame rendered, with the widget's device pixel ratio.
    [[nodiscard]] const QImage& frame() const { return frame_; }

  signals:
    // A frame was rendered.
    void frame_drawn();

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    void draw();

    const ViewRenderer* renderer_;
    QImage frame_;
    bool pending_ = false;
};

} // namespace cs::ui
