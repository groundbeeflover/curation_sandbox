// SPDX-License-Identifier: Apache-2.0

#include "common.hpp"

#include <cs/core/colour.hpp>
#include <cs/doc/project_io.hpp>
#include <cs/engine/path_tracer.hpp>
#include <cs/world/compile.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <thread>

namespace cs_render {

std::optional<Tier> parse_tier(std::string_view name) {
    if (name == "path") {
        return Tier::path;
    }
    if (name == "standard") {
        return Tier::standard;
    }
    return std::nullopt;
}

std::string_view tier_name(Tier tier) {
    return tier == Tier::standard ? "standard" : "path";
}

unsigned thread_count(unsigned requested) {
    return requested != 0 ? requested : std::max(1u, std::thread::hardware_concurrency());
}

Project load_project(const std::filesystem::path& folder) {
    cs::doc::Document document = cs::doc::load_project_folder(folder);
    cs::world::World world = cs::world::compile(document);
    for (const cs::world::Problem& problem : world.problems) {
        std::cerr << "cs-render: warning: " << problem.entity.to_string() << ": " << problem.reason
                  << '\n';
    }
    if (world.cameras.empty()) {
        throw std::runtime_error("the project has no viewpoint to render from");
    }
    cs::engine::LoadedTextures textures = cs::engine::load_textures(world, folder / "assets");
    for (const cs::engine::TextureProblem& problem : textures.problems) {
        std::cerr << "cs-render: warning: image " << problem.asset.sha256 << "."
                  << problem.asset.extension << " is shown as grey: " << problem.reason << '\n';
    }
    return Project{folder, std::move(document), std::move(world), std::move(textures)};
}

cs::Image render_view(const Project& project, const cs::engine::Scene& scene,
                      const ImageSettings& settings, cs::ThreadPool& pool) {
    cs::engine::RenderOptions options;
    options.width = settings.width;
    options.height = settings.height;
    options.samples_per_pixel = settings.samples_per_pixel;
    options.seed = settings.seed;
    options.direct_only = settings.direct_only;
    options.pool = &pool;
    options.textures = &project.textures.textures;
    return cs::engine::render(scene, project.camera(), options);
}

double mean_luminance(const cs::Image& image) {
    double sum = 0.0;
    double count = 0.0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const cs::Rgb& p = image.at(x, y);
            const double y_nits = cs::luminance(glm::dvec3(p.r, p.g, p.b));
            if (std::isfinite(y_nits)) {
                sum += y_nits;
                count += 1.0;
            }
        }
    }
    return count > 0.0 ? sum / count : 0.0;
}

double display_ev100(const cs::world::Camera& camera, const cs::Image& image) {
    return camera.ev100.has_value() ? *camera.ev100
                                    : cs::ev100_from_average_luminance(mean_luminance(image));
}

} // namespace cs_render
