// SPDX-License-Identifier: Apache-2.0
#include "viewport/view_renderer.hpp"

#include <numbers>
#include <stdexcept>
#include <utility>

namespace cs::ui {

world::Camera default_camera() {
    world::Camera camera;
    camera.position_m = {0.0, 0.0, 1.6};
    camera.forward = {1.0, 0.0, 0.0};
    camera.up = {0.0, 0.0, 1.0};
    camera.vertical_fov_rad = 50.0 * std::numbers::pi / 180.0;
    return camera;
}

ViewRenderer::ViewRenderer(const gpu::Device& device) : device_(&device) {}

ViewRenderer::~ViewRenderer() {
    drop_tier();
    // drop_tier waited for the device, so the scene is free to go.
}

std::vector<std::string> ViewRenderer::set_world(const world::World& world,
                                                 const std::filesystem::path& assets_folder) {
    drop_tier();
    scene_.reset();
    world_.reset();

    gpu::TextureLoader loader;
    if (assets_folder.empty()) {
        loader = [](const world::AssetRef&) -> assets::Texture {
            throw std::runtime_error("the project has no folder for its images yet");
        };
    } else {
        loader = gpu::project_texture_loader(assets_folder);
    }
    auto scene = std::make_unique<gpu::Scene>(*device_, world, loader);
    std::vector<std::string> problems;
    for (const gpu::TextureProblem& problem : scene->problems()) {
        problems.push_back("image " + problem.asset.sha256 + "." + problem.asset.extension +
                           " is shown as its base colour: " + problem.reason);
    }
    world_ = std::make_unique<world::World>(world);
    scene_ = std::move(scene);
    make_tier();
    return problems;
}

void ViewRenderer::set_size(std::uint32_t width, std::uint32_t height) {
    if (width == width_ && height == height_) {
        return;
    }
    drop_tier();
    width_ = width;
    height_ = height;
    make_tier();
}

void ViewRenderer::make_tier() {
    if (!scene_ || width_ == 0 || height_ == 0) {
        return;
    }
    try {
        tier_ =
            std::make_unique<gpu::StandardTier>(*device_, *scene_, world_->lights, width_, height_);
        pass_ = std::make_unique<gpu::ToneMapPass>(*device_, *tier_);
    } catch (...) {
        pass_.reset();
        tier_.reset();
        throw;
    }
}

void ViewRenderer::drop_tier() {
    if (!tier_ && !pass_) {
        return;
    }
    // A tier and its pass free everything at once: the frames that recorded them, and the
    // presenter's blit of the display image, must be done.
    device_->wait_idle();
    pass_.reset();
    tier_.reset();
}

void ViewRenderer::record(VkCommandBuffer commands) const {
    if (!ready()) {
        throw std::logic_error("ViewRenderer::record: nothing to draw yet");
    }
    tier_->record(commands, camera_);
    pass_->record(commands, ev100_);
}

gpu::PresentSource ViewRenderer::present_source() const {
    if (!ready()) {
        throw std::logic_error("ViewRenderer::present_source: nothing to draw yet");
    }
    gpu::PresentSource source;
    source.image = pass_->display_image();
    source.format = gpu::ToneMapPass::display_format;
    source.extent = {pass_->width(), pass_->height()};
    source.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    source.stages = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT;
    source.accesses = VK_ACCESS_2_TRANSFER_READ_BIT;
    return source;
}

std::vector<std::uint8_t> ViewRenderer::render() const {
    if (!ready()) {
        throw std::logic_error("ViewRenderer::render: nothing to draw yet");
    }
    // The tier's read-back of the HDR image is not needed, only its frame: render() leaves the
    // HDR image as the pass reads it.
    static_cast<void>(tier_->render(camera_));
    return pass_->render(ev100_);
}

} // namespace cs::ui
