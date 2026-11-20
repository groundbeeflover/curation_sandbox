// SPDX-License-Identifier: Apache-2.0
//
// What the viewport draws, without Qt: a world's Standard tier (M1-22) through the tone-mapping
// pass (M1-44), at one size, from one camera at one EV100. The two backends of the viewport use
// it: the window records it each frame before presenting the display image (M1-23), and the
// headless view renders it and reads the display image back.
//
// A StandardTier is made for one world and one size (its lights are uploaded once, its images have
// a fixed size), so a new world or a new size makes a new tier and a new pass, after waiting for
// the device to be idle: the presenter's blit may still be reading the old display image.
#pragma once

#include <cs/gpu/device.hpp>
#include <cs/gpu/presenter.hpp>
#include <cs/gpu/scene.hpp>
#include <cs/gpu/standard_tier.hpp>
#include <cs/gpu/tonemap.hpp>
#include <cs/world/world.hpp>

#include "viewport/exposure.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace cs::ui {

// The camera of a world without viewpoints: standing at the origin at eye height, looking along
// x with a 50 degree vertical field of view. Walking moves it (M1-27).
[[nodiscard]] world::Camera default_camera();

class ViewRenderer {
  public:
    // The device must outlive the renderer.
    explicit ViewRenderer(const gpu::Device& device);
    // Waits for the device to be idle, then frees the tier, the pass and the scene.
    ~ViewRenderer();

    ViewRenderer(const ViewRenderer&) = delete;
    ViewRenderer& operator=(const ViewRenderer&) = delete;
    ViewRenderer(ViewRenderer&&) = delete;
    ViewRenderer& operator=(ViewRenderer&&) = delete;

    // A new world, whose paintings' images are in assets_folder (a project's assets/); an empty
    // path loads none. Uploads its scene, and makes the tier and pass again at the current size.
    // Returns the paintings shown as their base colour instead of their image, one line each.
    // Throws as gpu::Scene and gpu::StandardTier do; the renderer then has no world.
    std::vector<std::string> set_world(const world::World& world,
                                       const std::filesystem::path& assets_folder);
    // The size in pixels. The same size changes nothing; a new one makes the tier and pass again.
    // A size of 0 drops them: nothing is drawn until a size with area comes. Throws as
    // gpu::StandardTier does.
    void set_size(std::uint32_t width, std::uint32_t height);

    void set_camera(const world::Camera& camera) {
        camera_ = camera;
        stale_ = true;
    }
    void set_ev100(double ev100) {
        ev100_ = ev100;
        stale_ = true;
    }
    [[nodiscard]] const world::Camera& camera() const { return camera_; }
    [[nodiscard]] double ev100() const { return ev100_; }

    // There is a world and a size with area, so there is something to draw.
    [[nodiscard]] bool ready() const { return pass_ != nullptr; }
    [[nodiscard]] std::uint32_t width() const { return width_; }
    [[nodiscard]] std::uint32_t height() const { return height_; }

    // Something changed since the last record(): the world, the size, the camera or the EV100.
    // Otherwise the display image still holds the last frame, and can be presented again as it is.
    [[nodiscard]] bool stale() const { return stale_; }
    // Records the tier and the pass into a command buffer that is recording (when ready()). The
    // display image is then in TRANSFER_SRC_OPTIMAL, as present_source() says. Throws
    // std::invalid_argument, before recording anything, for a camera the tier refuses.
    void record(VkCommandBuffer commands);
    // The display image, for Presenter::present() after the commands of record() were submitted.
    [[nodiscard]] gpu::PresentSource present_source() const;

    // Renders and reads the display image back (when ready()): width() * height() pixels of four
    // bytes, R, G, B sRGB-encoded and A = 255, row 0 at the top. Submits and waits.
    [[nodiscard]] std::vector<std::uint8_t> render() const;

  private:
    const gpu::Device* device_;
    std::vector<world::Light> lights_; // the world's: each new tier uploads them
    std::unique_ptr<gpu::Scene> scene_;
    std::unique_ptr<gpu::StandardTier> tier_;
    std::unique_ptr<gpu::ToneMapPass> pass_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    world::Camera camera_ = default_camera();
    double ev100_ = Exposure::default_ev100;
    bool stale_ = true;

    void make_tier();
    void drop_tier();
};

} // namespace cs::ui
