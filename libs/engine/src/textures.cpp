// SPDX-License-Identifier: Apache-2.0

#include <cs/engine/textures.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <set>
#include <stdexcept>
#include <string_view>
#include <variant>

namespace cs::engine {

namespace {

// The extensions an asset may have (world/ids.hpp).
constexpr std::array<std::string_view, 4> asset_extensions = {"png", "jpg", "tif", "exr"};

bool is_sha256(std::string_view text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

glm::dvec3 texel(const Image& image, int x, int y) {
    const Rgb& p = image.at(x, y);
    return {p.r, p.g, p.b};
}

// The texel centres either side of coordinate t (in texels from the edge) along a side of
// `size` texels, and the weight of the second, clamped to the edge.
struct Span {
    int first;
    int second;
    double weight;
};

Span span(double t, int size) {
    const double c = t - 0.5; // in texel centres
    if (!(c > 0.0)) {
        return {0, 0, 0.0};
    }
    if (!(c < static_cast<double>(size - 1))) {
        return {size - 1, size - 1, 0.0};
    }
    const double floor_c = std::floor(c);
    const int first = static_cast<int>(floor_c);
    return {first, first + 1, c - floor_c};
}

} // namespace

void Textures::add(const world::AssetRef& asset, assets::Texture texture) {
    if (texture.levels.empty()) {
        throw std::invalid_argument("textures: a texture has no levels");
    }
    textures_[{asset.sha256, asset.extension}] =
        std::make_shared<const assets::Texture>(std::move(texture));
}

const assets::Texture* Textures::find(const world::AssetRef& asset) const {
    const auto it = textures_.find({asset.sha256, asset.extension});
    return it == textures_.end() ? nullptr : it->second.get();
}

glm::dvec3 sample_bilinear(const assets::Texture& texture, glm::dvec2 uv) {
    const Image& image = texture.levels.front();
    if (!std::isfinite(uv.x) || !std::isfinite(uv.y)) {
        uv = {0.5, 0.5};
    }
    const int width = image.width();
    const int height = image.height();
    const Span x = span(uv.x * width, width);
    const Span y = span(uv.y * height, height);
    const glm::dvec3 top = (1.0 - x.weight) * texel(image, x.first, y.first) +
                           x.weight * texel(image, x.second, y.first);
    const glm::dvec3 bottom = (1.0 - x.weight) * texel(image, x.first, y.second) +
                              x.weight * texel(image, x.second, y.second);
    return (1.0 - y.weight) * top + y.weight * bottom;
}

glm::dvec3 painting_base_colour(const world::PaintingSurface& surface, glm::dvec2 uv,
                                const Textures* textures) {
    if (textures != nullptr && surface.image) {
        if (const assets::Texture* texture = textures->find(*surface.image)) {
            return sample_bilinear(*texture, uv);
        }
    }
    return surface.parameters.base.color;
}

std::filesystem::path asset_path(const std::filesystem::path& assets_dir,
                                 const world::AssetRef& asset) {
    if (!is_sha256(asset.sha256)) {
        throw std::invalid_argument(
            "asset: the name is not 64 lowercase hexadecimal digits (a SHA-256)");
    }
    if (std::find(asset_extensions.begin(), asset_extensions.end(), asset.extension) ==
        asset_extensions.end()) {
        throw std::invalid_argument("asset: the extension is not png, jpg, tif or exr");
    }
    return assets_dir / (asset.sha256 + "." + asset.extension);
}

LoadedTextures load_textures(const world::World& world, const std::filesystem::path& assets_dir) {
    LoadedTextures loaded;
    std::set<std::pair<std::string, std::string>> seen;
    for (const world::Material& material : world.materials) {
        const auto* surface = std::get_if<world::PaintingSurface>(&material);
        if (surface == nullptr || !surface->image) {
            continue;
        }
        const world::AssetRef& asset = *surface->image;
        if (!seen.insert({asset.sha256, asset.extension}).second) {
            continue;
        }
        try {
            loaded.textures.add(asset, assets::load_texture(asset_path(assets_dir, asset)));
        } catch (const std::exception& error) {
            loaded.problems.push_back({asset, error.what()});
        }
    }
    return loaded;
}

} // namespace cs::engine
