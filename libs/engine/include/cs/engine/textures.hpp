// SPDX-License-Identifier: Apache-2.0

#pragma once

// Painting textures for the engine
// --------------------------------
// The world names each work's photograph by its content hash (world::AssetRef) and holds no
// pixels. The engine reads the photographs from a Textures: the images of the project's assets
// folder, loaded once by the caller (cs-render, the app's lux service) and then only read, so any
// number of threads may sample them at once.
//
// The lux meter and the path tracer reflect light with the same MaterialBrdfs, so that a painting
// reflects the same light in a lux reading and in a picture. A painting's base colour is its
// photograph's texel at the hit, sampled from the texture's base mip (level 0) bilinearly: the
// engine's rays each see one point of the surface, so no filtering over a footprint is needed.
// Without a photograph, or without its texture, the material's base colour stands in (0.8 grey
// by default).

#include <cs/assets/texture.hpp>
#include <cs/material/brdf.hpp>
#include <cs/material/painting_material.hpp>
#include <cs/world/ids.hpp>
#include <cs/world/world.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
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
    std::map<std::pair<std::string, std::string>, assets::Texture> textures_; // (sha256, extension)
};

// The texture of a material's photograph: for a painting whose image `textures` holds, that
// texture; null for any other material, a painting without an image, or null textures. The lux
// meter and the path tracer look it up once per material, then sample it at every hit.
const assets::Texture* photograph_of(const world::Material& material, const Textures* textures);

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
// holds it; MaterialBrdfs clamps it to [0, 1] as it does every albedo. The 0.02 to 0.90 range of
// ADR 0003 (painting_material.hpp) belongs to the import, so that every tier sees the same texels,
// and is not applied yet.
glm::dvec3 painting_base_colour(const world::PaintingSurface& surface, glm::dvec2 uv,
                                const Textures* textures);

// The BRDF of every material of a world, as the lux meter and the path tracer reflect light: a
// Lambert surface with its albedo, a painting with its PaintingBrdf, whose base colour is its
// photograph's texel at the hit (above). Every albedo is clamped to [0, 1], NaN to 0: outside it,
// a surface would create energy, or a negative one. Built once per reading or render; it refers
// to the world's materials and to the textures, which must outlive it, and is then only read.
class MaterialBrdfs {
  public:
    // textures may be null: every painting then reflects its base colour.
    MaterialBrdfs(const world::World& world, const Textures* textures);

    // The BRDF of material `material` (an index into World::materials) at the hit's uv. For a
    // painting with a photograph it is made in `painting`, which must live as long as the
    // reference is used; otherwise it is the material's own, made once.
    const material::Brdf& at(std::uint32_t material, glm::dvec2 uv,
                             std::optional<material::PaintingBrdf>& painting) const;

  private:
    struct Entry {
        std::unique_ptr<material::Brdf> brdf;       // the material's own
        const material::PaintingMaterial* painting; // a painting's parameters, or null
        const assets::Texture* photograph;          // its photograph's texture, or null
    };
    std::vector<Entry> entries_; // one per material
};

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
