// SPDX-License-Identifier: Apache-2.0
//
// The viewport adapter (ADR 0006, ADR 0011): the widget the main window shows, drawing the open
// project's Standard tier through the tone-mapping pass.
//
// It has two backends, chosen when it is made:
// - On a platform with window surfaces, a QWindow of type VulkanSurface embedded with
//   QWidget::createWindowContainer, presenting through cs::gpu::Presenter (vulkan_window.hpp).
// - On the offscreen platform, which has no Vulkan surface, a plain widget that paints the display
//   image read back from the GPU (headless_view.hpp), so that UI tests run headless and their
//   screenshots show the viewport.
//
// The viewport draws on demand: when the world, the camera, the exposure or the size changes.
// A change of size resizes the swapchain at once, but makes the Standard tier again only once the
// size has settled (settle_ms without another change), since each tier builds its pipeline and
// uploads the lights; until then the last frame is stretched to the new size.
//
// Walking (M1-27) uses its relative pointer mode, through PointerLock (M1-45): lock() activates the
// viewport's window first, so that Esc reaches it.
#pragma once

#include "viewport/exposure.hpp"

#include <cs/world/world.hpp>

#include <QImage>
#include <QTimer>
#include <QWidget>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class QWindow;

namespace cs::ui {

class GpuContext;
class HeadlessView;
class PointerLock;
class ViewRenderer;
class VulkanWindow;

// Whether the Qt platform makes Vulkan surfaces for windows: false on offscreen (and minimal),
// where the viewport is headless.
[[nodiscard]] bool platform_has_vulkan_surfaces();

class Viewport : public QWidget {
    Q_OBJECT
  public:
    // How long the size must stay unchanged before the tier is made again at it.
    static constexpr int settle_ms = 150;

    // The GPU must outlive the viewport. headless: draw into an image instead of a window
    // surface; the offscreen platform has no surface, so make_main_window passes true there.
    Viewport(GpuContext& gpu, bool headless, QWidget* parent = nullptr);
    ~Viewport() override;

    Viewport(const Viewport&) = delete;
    Viewport& operator=(const Viewport&) = delete;
    Viewport(Viewport&&) = delete;
    Viewport& operator=(Viewport&&) = delete;

    [[nodiscard]] bool headless() const { return headless_view_ != nullptr; }

    // ---- What it shows ----------------------------------------------------------------------

    // A new world (the open project's), whose paintings' images are in assets_folder; an empty
    // path loads none. Its first viewpoint becomes active, if it has one; otherwise the camera
    // goes to default_camera() and the EV100 stays. Returns the problems found uploading it, one
    // line each (paintings shown as their base colour); throws as gpu::Scene does.
    std::vector<std::string> set_world(const world::World& world,
                                       const std::filesystem::path& assets_folder);
    // A viewpoint becomes active: its camera, and its EV100 if it has one (Exposure::view_from).
    void show_viewpoint(const world::Camera& viewpoint);
    // The camera moves on its own, no viewpoint active: the EV100 stays.
    void set_camera(const world::Camera& camera);
    // A new project: EV100 8 (Exposure::reset), then set_world.
    void reset_exposure();

    [[nodiscard]] const world::Camera& camera() const;
    [[nodiscard]] double ev100() const { return exposure_.ev100(); }

    // The size the tier renders at, in device pixels: {0, 0} until the first one has settled.
    [[nodiscard]] QSize render_size() const;
    // Frames drawn so far: presented by the window, or rendered by the headless view.
    [[nodiscard]] int frames() const { return frames_; }
    // Swapchains the window has made: the first, then one per change of size or surface. 0 when
    // headless.
    [[nodiscard]] std::uint32_t swapchains_created() const;
    // The last frame drawn, with the widget's device pixel ratio; a null image before the first.
    // Just after a new size settles it may still be at the old size, until the next frame. The
    // window copies it back from its swapchain, which may cost a frame.
    [[nodiscard]] QImage grab_frame();

    // ---- Walking: relative pointer mode -----------------------------------------------------

    // Starts walk mode on the viewport's window. False, changing nothing, when the window is not
    // shown, or on Wayland without the protocols (PointerLock::lock).
    bool lock_pointer();
    void unlock_pointer();
    [[nodiscard]] bool pointer_locked() const;

  signals:
    void ev100_changed(double ev100);
    // A frame was drawn.
    void frame_drawn();
    // Walk mode: the pointer is held, released, and moved by (dx, dy) logical pixels, x to the
    // right and y down (PointerLock).
    void pointer_locked_changed(bool locked);
    void pointer_motion(double dx, double dy);

  protected:
    bool event(QEvent* event) override;

  private:
    // After the exposure changed from `before`: the renderer follows, ev100_changed when it moved.
    void exposure_changed(double before);
    void size_changed();
    void settle_size();
    void request_frame();
    void frame_done();
    QWindow* pointer_window() const;
    PointerLock& pointer_lock();

    Exposure exposure_;
    std::unique_ptr<ViewRenderer> renderer_;
    QTimer settle_timer_;
    int frames_ = 0;
    HeadlessView* headless_view_ = nullptr; // a child, when headless
    VulkanWindow* window_ = nullptr;        // in a child container, otherwise
    QWidget* container_ = nullptr;
    std::unique_ptr<PointerLock> pointer_lock_;
};

} // namespace cs::ui
