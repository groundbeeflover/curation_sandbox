// SPDX-License-Identifier: Apache-2.0

#pragma once

// Painting textures for the engine
// --------------------------------
// The world names each work's photograph by its content hash (world::AssetRef) and holds no
// pixels. The engine reads the photographs from a Textures: the images of the project's assets
// folder, loaded once by the caller (cs-render, the app's lux service) and then only read, so any
// number of threads may sample them at once.
//
// The lux meter and the path tracer find a painting's base colour, texel by texel, through one
// function, painting_base_colour, so that the light a painting reflects is the same in a lux
// reading and in a picture. It samples the texture's base mip (level 0) bilinearly: the engine's
// rays each see one point of the surface, so no filtering over a footprint is needed. Without a
// photograph, or without its texture, the material's base colour stands in (0.8 grey by default).

#include <cs/assets/texture.hpp>
#include <cs/world/ids.hpp>
#include <cs/world/world.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cs::engine {

// Textures by asset.
class Textures {
  public:
    // Adds the texture of an asset, replacing any it had. Throws std::invalid_argument if the
    // texture has no levels.
    void add(const world::AssetRef& asset, assets::Texture texture);

    // The texture of an asset, or null when it has none.
    const assets::Texture* find(const world::AssetRef& asset) const;

    std::size_t size() const { return textures_.size(); }

  private:
    // By sha256, then extension.
    std::map<std::pair<std::string, std::string>, std::shared_ptr<const assets::Texture>> textures_;
};

// The texture's base level at uv, bilinearly between the four nearest texel centres. uv is in
// image coordinates, as the painting mesh's (geom/painting.hpp): (0, 0) the top-left corner of
// the image, (1, 1) its bottom-right, u to the right and v down. Texel (x, y) has its centre at
// ((x + 0.5) / width, (y + 0.5) / height), where it reads exactly that texel. Outside the texel
// centres the edge texels are repeated (clamp to edge); a uv that is not finite reads as the
// centre of the image. Linear Rec. 709, as the texture holds it.
glm::dvec3 sample_bilinear(const assets::Texture& texture, glm::dvec2 uv);

// A painting's base colour at uv (as above): its photograph's texel, sampled with
// sample_bilinear, when the surface has an image and `textures` holds it; else
// surface.parameters.base.color. textures may be null. The value is the albedo as the texture
// holds it; the lux meter and path tracer clamp it to [0, 1] as they do every albedo.
glm::dvec3 painting_base_colour(const world::PaintingSurface& surface, glm::dvec2 uv,
                                const Textures* textures);

// The file of an asset in a project's assets folder: <assets_dir>/<sha256>.<extension>. Throws
// std::invalid_argument unless sha256 is 64 lowercase hexadecimal digits and the extension is
// one of "png", "jpg", "tif" and "exr", so that a project file cannot name a path outside the
// folder.
std::filesystem::path asset_path(const std::filesystem::path& assets_dir,
                                 const world::AssetRef& asset);

// A photograph the world names that load_textures could not load, and why.
struct TextureProblem {
    world::AssetRef asset;
    std::string reason;
};

struct LoadedTextures {
    Textures textures;
    std::vector<TextureProblem> problems; // in the order of the world's materials
};

// Loads the photograph of every painting of the world from assets_dir, each asset once, with
// assets::load_texture. An asset whose name is not valid (asset_path), whose file is missing or
// cannot be decoded is left out and reported in problems; its painting then shows its base
// colour. Never throws for a missing or bad file.
LoadedTextures load_textures(const world::World& world, const std::filesystem::path& assets_dir);

} // namespace cs::engine
