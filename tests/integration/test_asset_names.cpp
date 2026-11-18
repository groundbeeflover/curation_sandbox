// SPDX-License-Identifier: Apache-2.0

// One check for asset names (issue #154, M1-43): the engine (engine::load_textures) and the GPU
// (gpu::Scene through gpu::project_texture_loader) load a project's photographs by the same
// assets::asset_file, so for any list of names they load the same ones. Before, the engine took
// png, jpg, tif and exr and the GPU png, jpg and jpeg: a .jpeg photograph showed on screen while
// the lux meter read its painting as plain grey. It belongs here because it compares two modules.
//
// Every name, valid or hostile, has a real image in the file it would name, so a tier that
// skipped the check would load it and disagree. The hostile names are tested one by one in
// libs/assets/tests/test_asset_file.cpp; here one or two of each kind stand for them.
//
// Validation messages fail this test through gpu_validation.cpp's listener; devices come from
// libs/gpu/tests/gpu_test.hpp.

#include "../../libs/gpu/tests/gpu_test.hpp"

#include <cs/assets/texture.hpp>
#include <cs/core/image.hpp>
#include <cs/engine/textures.hpp>
#include <cs/gpu/device.hpp>
#include <cs/gpu/scene.hpp>
#include <cs/world/ids.hpp>
#include <cs/world/world.hpp>

#include <catch2/catch_test_macros.hpp>

// The test writes its images in memory with stb_image_write. STATIC keeps its functions inside
// this file, apart from core's copy.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using cs::world::AssetRef;

namespace {

// Textures are half floats on the GPU (gpu/scene.hpp): a linear value in [0, 1] comes back within
// 4.9e-4 of the engine's. 1e-3 allows that; another image, or none, is off by far more.
constexpr float half_float_tolerance = 1e-3f;

using Bytes = std::vector<char>;

void append_bytes(void* context, void* data, int size) {
    auto* out = static_cast<Bytes*>(context);
    const auto* begin = static_cast<const char*>(data);
    out->insert(out->end(), begin, begin + size);
}

// A 2 x 2 image of four colours, as a PNG and as a JPEG.
const std::vector<std::uint8_t> four_pixels = {200, 40, 40,  40,  200, 40,
                                               40,  40, 200, 128, 128, 128};

Bytes png() {
    Bytes out;
    REQUIRE(stbi_write_png_to_func(append_bytes, &out, 2, 2, 3, four_pixels.data(), 2 * 3) != 0);
    return out;
}

Bytes jpeg() {
    Bytes out;
    REQUIRE(stbi_write_jpg_to_func(append_bytes, &out, 2, 2, 3, four_pixels.data(), 100) != 0);
    return out;
}

// Writes `image` to the file `path` names, as the file system resolves it: every directory the
// path passes through is made first, and a NUL ends the name where the file system reads it.
void plant(const fs::path& path, const Bytes& image) {
    fs::path prefix;
    for (auto it = path.begin(); it != path.end() && std::next(it) != path.end(); ++it) {
        prefix /= *it;
        if (*it != "." && *it != ".." && !fs::exists(prefix)) {
            fs::create_directory(prefix);
        }
    }
    std::ofstream file(path, std::ios::binary);
    REQUIRE(file);
    file.write(image.data(), static_cast<std::streamsize>(image.size()));
    REQUIRE(file);
}

// A directory of its own under the working directory, which ctest sets to this build's directory.
// Emptied when made and removed when the test ends.
class TempDir {
  public:
    explicit TempDir(const std::string& name)
        : path_(fs::current_path() / ("cs_integration_test_asset_names_" + name)) {
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return path_; }

  private:
    fs::path path_;
};

struct Name {
    const char* why;
    AssetRef asset;
    bool valid;
    bool is_jpeg = false; // the file holds a JPEG rather than a PNG
};

std::string hash(char digit) {
    return std::string(64, digit);
}

float max_abs_difference(const cs::Image& a, const cs::Image& b) {
    REQUIRE(a.width() == b.width());
    REQUIRE(a.height() == b.height());
    float worst = 0.0f;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            worst = std::max({worst, std::abs(a.at(x, y).r - b.at(x, y).r),
                              std::abs(a.at(x, y).g - b.at(x, y).g),
                              std::abs(a.at(x, y).b - b.at(x, y).b)});
        }
    }
    return worst;
}

} // namespace

TEST_CASE("asset names: engine::load_textures and gpu::Scene load the same photographs, and a "
          ".jpeg one in both",
          "[integration][gpu]") {
    const TempDir root("agree");
    const fs::path folder = root.path() / "assets";
    fs::create_directories(folder);

    std::string nul_inside = hash('a');
    nul_inside[20] = '\0';
    const std::vector<Name> names = {
        {"a PNG", {hash('1'), "png"}, true},
        {"a JPEG named .jpg", {hash('2'), "jpg"}, true, true},
        {"a JPEG named .jpeg", {hash('3'), "jpeg"}, true, true},
        // Extensions the engine took and could not decode, with a PNG inside that it could.
        {"tif (M2-32)", {hash('4'), "tif"}, false},
        {"exr", {hash('5'), "exr"}, false},
        {"an extension in upper case", {hash('6'), "PNG"}, false},
        {"a hash in upper case", {hash('B'), "png"}, false},
        {"a hash of 63 digits", {std::string(63, '7'), "png"}, false},
        {"a hash that leaves the folder", {"../" + std::string(61, '8'), "png"}, false},
        {"an extension that leaves the folder", {hash('9'), "png/../../x"}, false},
        {"a NUL in the hash", {nul_inside, "png"}, false},
        {"an absolute path", {(root.path() / "absolute").string(), "png"}, false},
    };

    // One painting per name, in that order; the scene needs no meshes to load its textures.
    cs::world::World world;
    for (const Name& name : names) {
        INFO(name.why);
        const fs::path unchecked = folder / (name.asset.sha256 + "." + name.asset.extension);
        plant(unchecked, name.is_jpeg ? jpeg() : png());
        // A real image where the name points: a tier that skipped the check would load it.
        REQUIRE_NOTHROW(cs::assets::load_texture(unchecked));

        cs::world::PaintingSurface painting;
        painting.image = name.asset;
        world.materials.emplace_back(painting);
    }

    const cs::engine::LoadedTextures engine = cs::engine::load_textures(world, folder);

    const auto gpu = cs::gpu::test::make_device();
    const cs::gpu::Scene scene(*gpu, world, cs::gpu::project_texture_loader(folder));
    REQUIRE(scene.material_data().size() == names.size());

    for (std::size_t i = 0; i < names.size(); ++i) {
        const Name& name = names[i];
        INFO(name.why);
        const cs::assets::Texture* in_engine = engine.textures.find(name.asset);
        const std::uint32_t on_gpu = scene.material_data()[i].texture;
        CHECK((in_engine != nullptr) == (on_gpu != cs::gpu::no_texture));
        CHECK((in_engine != nullptr) == name.valid);

        // The same photograph in both: the GPU's base level reads back as the engine's.
        if (in_engine != nullptr && on_gpu != cs::gpu::no_texture) {
            CHECK(max_abs_difference(scene.download_texture(on_gpu, 0), in_engine->levels[0]) <=
                  half_float_tolerance);
        }
    }

    // Each tier reports the same photographs it could not load, in the order of the materials.
    REQUIRE(engine.problems.size() == scene.problems().size());
    for (std::size_t i = 0; i < engine.problems.size(); ++i) {
        CHECK(engine.problems[i].asset == scene.problems()[i].asset);
    }
}
