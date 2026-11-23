// SPDX-License-Identifier: Apache-2.0

#include "standard.hpp"

#include <cs/gpu/scene.hpp>
#include <cs/gpu/standard_tier.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace cs_render {
namespace {

// A driver's name, from the device, is bounded before it names a file.
constexpr std::size_t max_driver_label_length = 64;

} // namespace

bool gpu_required() {
    return cs::gpu::gpu_required();
}

cs::gpu::DeviceOptions device_options() {
    cs::gpu::DeviceOptions options;
#ifndef NDEBUG
    if (gpu_required()) {
        options.validation = cs::gpu::Validation::required;
    }
#endif
    return options;
}

std::string driver_label(const cs::gpu::DeviceReport& report) {
    if (report.driver == "llvmpipe") {
        return "lavapipe";
    }
    std::string label;
    bool gap = false; // other characters since the last letter or digit
    for (const char c : report.driver) {
        const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        if (!((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9'))) {
            gap = !label.empty();
            continue;
        }
        if (label.size() + (gap ? 2 : 1) > max_driver_label_length) {
            break;
        }
        if (gap) {
            label += '-';
            gap = false;
        }
        label += lower;
    }
    return label.empty() ? "unknown" : label;
}

ValidationCounter::ValidationCounter() {
    previous_ = cs::gpu::set_validation_handler([this](const cs::gpu::ValidationMessage& message) {
        using Message = cs::gpu::ValidationMessage;
        std::cerr << cs::gpu::to_string(message) << '\n';
        if (message.severity >= Message::Severity::warning &&
            message.kind != Message::Kind::general) {
            count_.fetch_add(1);
        }
    });
}

ValidationCounter::~ValidationCounter() {
    cs::gpu::set_validation_handler(std::move(previous_));
}

void throw_if_reported(const ValidationCounter& validation) {
    if (validation.count() > 0) {
        throw std::runtime_error("the validation layer reported " +
                                 std::to_string(validation.count()) +
                                 " warnings or errors (above); any one fails a GPU test");
    }
}

cs::Image render_standard(const cs::gpu::Device& device, const Project& project, int width,
                          int height) {
    const cs::gpu::Scene scene(device, project.world,
                               cs::gpu::project_texture_loader(project.folder / "assets"));
    for (const cs::gpu::TextureProblem& problem : scene.problems()) {
        std::cerr << "cs-render: warning: image " << problem.asset.sha256 << "."
                  << problem.asset.extension
                  << " is shown as its base colour on the GPU: " << problem.reason << '\n';
    }
    const cs::gpu::StandardTier tier(device, scene, project.world.lights,
                                     static_cast<std::uint32_t>(width),
                                     static_cast<std::uint32_t>(height));
    return tier.render(project.camera()).radiance;
}

} // namespace cs_render
