// SPDX-License-Identifier: Apache-2.0

// Painting textures in the engine (textures.hpp), from issue #81 (M1-17) and the owner's comments
// on it: the engine samples a painting's photograph bilinearly on its base mip, through one
// function (painting_base_colour) that the lux meter and the path tracer share; without a
// photograph or its texture the material's base colour stands in. The acceptance test renders a
// 2 x 2-texel texture face-on and finds each texel's colour in its quadrant, the right way round.
//
// Asset names come from project files, which are hostile (CLAUDE.md): asset_path accepts only 64
// lowercase hexadecimal digits and a known extension, and load_textures reports what it cannot
// load instead of throwing.
//
// Written from the issue and the public headers only (test-author), without the implementation.

#include <cs/engine/lux.hpp>
#include <cs/engine/path_tracer.hpp>
#include <cs/engine/scene.hpp>
#include <cs/engine/textures.hpp>

#include <cs/assets/texture.hpp>
#include <cs/core/image.hpp>
#include <cs/core/random.hpp>
#include <cs/geom/mesh.hpp>
#include <cs/geom/painting.hpp>
#include <cs/geom/wall_face.hpp>
#include <cs/light/distribution.hpp>
#include <cs/light/luminaire.hpp>
#include <cs/world/ids.hpp>
#include <cs/world/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

// The tests write their PNG files in memory with stb_image_write, as libs/assets/tests does.
// STATIC keeps its functions inside this file, apart from core's copy.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using cs::engine::asset_path;
using cs::engine::load_textures;
using cs::engine::LoadedTextures;
using cs::engine::painting_base_colour;
using cs::engine::sample_bilinear;
using cs::engine::Textures;
using glm::dvec2;
using glm::dvec3;

namespace assets = cs::assets;
namespace geom = cs::geom;
namespace world = cs::world;

namespace {

constexpr double not_a_number = std::numeric_limits<double>::quiet_NaN();
constexpr double inf = std::numeric_limits<double>::infinity();

// Texels are floats and sample_bilinear returns doubles: an implementation may interpolate in
// either. Values here are at most 1, so float rounding of a weighted sum of four texels is below
// 4 x 6e-8; 1e-6 allows that and catches any real error in the weights (the smallest weight step
// the tests use changes a value by more than 1e-3).
constexpr double texel_tolerance = 1e-6;

// 64 lowercase hexadecimal digits: a valid asset name.
const std::string valid_sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const std::string other_sha = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

dvec3 rgb_of(const cs::Rgb& p) {
    return {p.r, p.g, p.b};
}

void set(cs::Image& image, int x, int y, dvec3 c) {
    image.at(x, y) =
        cs::Rgb{static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z)};
}

// A texture whose level 0 is `image` and whose further levels are its mip chain.
assets::Texture texture_of(const cs::Image& image) {
    return assets::Texture{assets::build_mip_chain(image)};
}

// A 3 x 2 image of six unrelated colours, so that a swap of x and y, a mirror or a wrong weight
// shows. Not a bilinear function of position, so only the four nearest texels give the right
// answer between them.
cs::Image three_by_two() {
    cs::Image image(3, 2);
    set(image, 0, 0, {0.9, 0.1, 0.4});
    set(image, 1, 0, {0.2, 0.7, 0.3});
    set(image, 2, 0, {0.5, 0.5, 0.0});
    set(image, 0, 1, {0.0, 0.3, 0.8});
    set(image, 1, 1, {0.6, 0.2, 0.9});
    set(image, 2, 1, {1.0, 0.8, 0.1});
    return image;
}

// The specification of sample_bilinear written out (textures.hpp): texel (x, y) has its centre at
// ((x + 0.5) / width, (y + 0.5) / height); between centres, the four nearest are weighted
// bilinearly; outside them the edge texels repeat.
dvec3 reference_bilinear(const cs::Image& image, dvec2 uv) {
    const int w = image.width();
    const int h = image.height();
    const double x = std::clamp(uv.x * w - 0.5, 0.0, static_cast<double>(w - 1));
    const double y = std::clamp(uv.y * h - 0.5, 0.0, static_cast<double>(h - 1));
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(x0 + 1, w - 1);
    const int y1 = std::min(y0 + 1, h - 1);
    const double fx = x - x0;
    const double fy = y - y0;
    return (1.0 - fx) * (1.0 - fy) * rgb_of(image.at(x0, y0)) +
           fx * (1.0 - fy) * rgb_of(image.at(x1, y0)) + (1.0 - fx) * fy * rgb_of(image.at(x0, y1)) +
           fx * fy * rgb_of(image.at(x1, y1));
}

void check_colour(dvec3 actual, dvec3 expected, double tolerance = texel_tolerance) {
    INFO("actual (" << actual.x << ", " << actual.y << ", " << actual.z << "), expected ("
                    << expected.x << ", " << expected.y << ", " << expected.z << ")");
    CHECK_THAT(actual.x, WithinAbs(expected.x, tolerance));
    CHECK_THAT(actual.y, WithinAbs(expected.y, tolerance));
    CHECK_THAT(actual.z, WithinAbs(expected.z, tolerance));
}

} // namespace

// ---- sample_bilinear ---------------------------------------------------------------------------

TEST_CASE("sample_bilinear: at a texel's centre it reads exactly that texel",
          "[engine][textures]") {
    const cs::Image image = three_by_two();
    const assets::Texture texture = texture_of(image);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 3; ++x) {
            INFO("texel (" << x << ", " << y << ")");
            const dvec2 centre((x + 0.5) / 3.0, (y + 0.5) / 2.0);
            check_colour(sample_bilinear(texture, centre), rgb_of(image.at(x, y)));
        }
    }
}

TEST_CASE("sample_bilinear: half way between two texel centres it reads their average, and at "
          "the middle of four their average",
          "[engine][textures][accuracy]") {
    const cs::Image image = three_by_two();
    const assets::Texture texture = texture_of(image);
    const auto t = [&](int x, int y) { return rgb_of(image.at(x, y)); };

    // Between (0, 0) and (1, 0): u = (0.5 + 1.5) / 2 / 3 = 1/3, v = 0.25 (row 0's centre).
    check_colour(sample_bilinear(texture, {1.0 / 3.0, 0.25}), 0.5 * (t(0, 0) + t(1, 0)));
    // Between (1, 0) and (1, 1): u = 0.5 (column 1's centre), v = 0.5.
    check_colour(sample_bilinear(texture, {0.5, 0.5}), 0.5 * (t(1, 0) + t(1, 1)));
    // The middle of (1, 0), (2, 0), (1, 1) and (2, 1): u = 2/3, v = 0.5.
    check_colour(sample_bilinear(texture, {2.0 / 3.0, 0.5}),
                 0.25 * (t(1, 0) + t(2, 0) + t(1, 1) + t(2, 1)));
    // A quarter of the way from (0, 1)'s centre to (1, 1)'s: u = (0.5 + 0.25) / 3, v = 0.75.
    check_colour(sample_bilinear(texture, {0.75 / 3.0, 0.75}), 0.75 * t(0, 1) + 0.25 * t(1, 1));
}

TEST_CASE("sample_bilinear: anywhere in and around the image it follows the bilinear rule",
          "[engine][textures][accuracy]") {
    // 1000 points in [-0.25, 1.25]^2, seeded, against the rule written out above.
    const cs::Image image = three_by_two();
    const assets::Texture texture = texture_of(image);
    for (std::uint32_t i = 0; i < 1000; ++i) {
        cs::Rng rng(cs::hash_seed(81, i, 0, 0, 0x7465'7801));
        const dvec2 uv(-0.25 + 1.5 * rng.next_double(), -0.25 + 1.5 * rng.next_double());
        INFO("uv (" << uv.x << ", " << uv.y << ")");
        check_colour(sample_bilinear(texture, uv), reference_bilinear(image, uv));
    }
}

TEST_CASE("sample_bilinear: outside the texel centres the edge texels repeat (clamp to edge)",
          "[engine][textures]") {
    const cs::Image image = three_by_two();
    const assets::Texture texture = texture_of(image);
    const auto t = [&](int x, int y) { return rgb_of(image.at(x, y)); };

    // The image's corners lie outside every texel centre: each reads its corner texel alone.
    check_colour(sample_bilinear(texture, {0.0, 0.0}), t(0, 0));
    check_colour(sample_bilinear(texture, {1.0, 0.0}), t(2, 0));
    check_colour(sample_bilinear(texture, {0.0, 1.0}), t(0, 1));
    check_colour(sample_bilinear(texture, {1.0, 1.0}), t(2, 1));
    // Far outside the image, too: no wrapping, no black border.
    check_colour(sample_bilinear(texture, {-3.0, -0.5}), t(0, 0));
    check_colour(sample_bilinear(texture, {7.0, -2.0}), t(2, 0));
    check_colour(sample_bilinear(texture, {-1.0, 5.0}), t(0, 1));
    check_colour(sample_bilinear(texture, {1.5, 1.5}), t(2, 1));
    // Clamped in one direction, interpolated in the other: left of the image at v = 0.5 is half
    // way between column 0's two texels.
    check_colour(sample_bilinear(texture, {-0.2, 0.5}), 0.5 * (t(0, 0) + t(0, 1)));
    // Above the image, half way between (1, 0) and (2, 0).
    check_colour(sample_bilinear(texture, {2.0 / 3.0, -4.0}), 0.5 * (t(1, 0) + t(2, 0)));
    // Inside the image but outside the centres: the first sixth of a 3-wide row is texel 0's.
    check_colour(sample_bilinear(texture, {0.1, 0.1}), t(0, 0));
}

TEST_CASE("sample_bilinear: a uv that is not finite reads the centre of the image",
          "[engine][textures]") {
    // textures.hpp: "a uv that is not finite reads as the centre of the image", the whole uv,
    // whichever of its components is not finite. For the 3 x 2 image the centre (0.5, 0.5) is
    // column 1's centre, half way between its two rows.
    const cs::Image image = three_by_two();
    const assets::Texture texture = texture_of(image);
    const dvec3 centre = 0.5 * (rgb_of(image.at(1, 0)) + rgb_of(image.at(1, 1)));
    check_colour(sample_bilinear(texture, {0.5, 0.5}), centre);

    for (const dvec2 uv :
         {dvec2(not_a_number, not_a_number), dvec2(not_a_number, 0.1), dvec2(0.9, not_a_number),
          dvec2(inf, 0.2), dvec2(0.3, -inf), dvec2(-inf, inf)}) {
        INFO("uv (" << uv.x << ", " << uv.y << ")");
        const dvec3 c = sample_bilinear(texture, uv);
        CHECK(std::isfinite(c.x));
        CHECK(std::isfinite(c.y));
        CHECK(std::isfinite(c.z));
        check_colour(c, centre);
    }
}

TEST_CASE("sample_bilinear: a 1 x 1 texture reads its one texel everywhere", "[engine][textures]") {
    cs::Image image(1, 1);
    set(image, 0, 0, {0.25, 0.5, 0.75});
    const assets::Texture texture = texture_of(image);
    for (const dvec2 uv : {dvec2(0.5, 0.5), dvec2(0.0, 0.0), dvec2(1.0, 1.0), dvec2(0.13, 0.91),
                           dvec2(-2.0, 3.0), dvec2(not_a_number, 0.5)}) {
        INFO("uv (" << uv.x << ", " << uv.y << ")");
        check_colour(sample_bilinear(texture, uv), {0.25, 0.5, 0.75});
    }
}

TEST_CASE("sample_bilinear: reads the base level, never a smaller mip", "[engine][textures]") {
    // Level 1 here is deliberately not the average of level 0, so that reading it shows.
    cs::Image base(2, 2);
    set(base, 0, 0, {0.1, 0.2, 0.3});
    set(base, 1, 0, {0.4, 0.5, 0.6});
    set(base, 0, 1, {0.7, 0.8, 0.9});
    set(base, 1, 1, {0.3, 0.3, 0.3});
    cs::Image small(1, 1);
    set(small, 0, 0, {1.0, 0.0, 1.0});
    const assets::Texture texture{{base, small}};

    check_colour(sample_bilinear(texture, {0.25, 0.25}), {0.1, 0.2, 0.3});
    check_colour(sample_bilinear(texture, {0.75, 0.75}), {0.3, 0.3, 0.3});
    check_colour(sample_bilinear(texture, {0.5, 0.5}),
                 0.25 * (dvec3(0.1, 0.2, 0.3) + dvec3(0.4, 0.5, 0.6) + dvec3(0.7, 0.8, 0.9) +
                         dvec3(0.3, 0.3, 0.3)));
}

// ---- Textures ----------------------------------------------------------------------------------

TEST_CASE("Textures: find returns the texture added for an asset, and null for any other",
          "[engine][textures]") {
    Textures textures;
    CHECK(textures.size() == 0);
    const world::AssetRef ref{valid_sha, "png"};
    CHECK(textures.find(ref) == nullptr);

    cs::Image image(2, 1);
    set(image, 0, 0, {0.2, 0.4, 0.6});
    textures.add(ref, texture_of(image));
    CHECK(textures.size() == 1);
    const assets::Texture* found = textures.find(ref);
    REQUIRE(found != nullptr);
    REQUIRE_FALSE(found->levels.empty());
    CHECK(found->width() == 2);
    CHECK(found->height() == 1);
    CHECK(found->levels[0].at(0, 0).g == 0.4f);

    // An asset is its hash and its extension (AssetRef::operator==): either differing is
    // another asset.
    CHECK(textures.find(world::AssetRef{other_sha, "png"}) == nullptr);
    CHECK(textures.find(world::AssetRef{valid_sha, "jpg"}) == nullptr);
    CHECK(textures.find(world::AssetRef{}) == nullptr);
}

TEST_CASE("Textures: adding an asset again replaces its texture", "[engine][textures]") {
    Textures textures;
    const world::AssetRef ref{valid_sha, "png"};
    cs::Image first(1, 1);
    set(first, 0, 0, {0.1, 0.1, 0.1});
    cs::Image second(1, 1);
    set(second, 0, 0, {0.9, 0.9, 0.9});
    textures.add(ref, texture_of(first));
    textures.add(ref, texture_of(second));
    CHECK(textures.size() == 1);
    REQUIRE(textures.find(ref) != nullptr);
    CHECK(textures.find(ref)->levels[0].at(0, 0).r == 0.9f);

    textures.add(world::AssetRef{other_sha, "png"}, texture_of(first));
    CHECK(textures.size() == 2);
}

TEST_CASE("Textures: a texture with no levels is refused with std::invalid_argument",
          "[engine][textures]") {
    Textures textures;
    const world::AssetRef ref{valid_sha, "png"};
    CHECK_THROWS_AS(textures.add(ref, assets::Texture{}), std::invalid_argument);
    CHECK(textures.size() == 0);
    CHECK(textures.find(ref) == nullptr);
}

// ---- painting_base_colour ----------------------------------------------------------------------

TEST_CASE("painting_base_colour: without a photograph, or without its texture, the base colour "
          "stands in",
          "[engine][textures]") {
    world::PaintingSurface surface;
    surface.parameters.base.color = {0.3, 0.5, 0.7};
    const dvec3 base = surface.parameters.base.color;

    cs::Image image(2, 2);
    set(image, 0, 0, {0.9, 0.0, 0.0});
    set(image, 1, 0, {0.0, 0.9, 0.0});
    set(image, 0, 1, {0.0, 0.0, 0.9});
    set(image, 1, 1, {0.1, 0.1, 0.1});
    Textures textures;
    textures.add(world::AssetRef{valid_sha, "png"}, texture_of(image));

    const dvec2 uv(0.25, 0.25);
    SECTION("no image on the surface") {
        check_colour(painting_base_colour(surface, uv, nullptr), base);
        check_colour(painting_base_colour(surface, uv, &textures), base);
    }
    SECTION("an image, but no textures") {
        surface.image = world::AssetRef{valid_sha, "png"};
        check_colour(painting_base_colour(surface, uv, nullptr), base);
    }
    SECTION("an image the textures do not hold") {
        surface.image = world::AssetRef{other_sha, "png"};
        check_colour(painting_base_colour(surface, uv, &textures), base);
        const Textures empty;
        surface.image = world::AssetRef{valid_sha, "png"};
        check_colour(painting_base_colour(surface, uv, &empty), base);
    }
    SECTION("the default material's 0.8 grey") {
        const world::PaintingSurface plain{};
        check_colour(painting_base_colour(plain, uv, &textures), dvec3(0.8));
    }
}

TEST_CASE("painting_base_colour: with its texture, a painting's base colour is the photograph's "
          "texel, sampled bilinearly",
          "[engine][textures]") {
    world::PaintingSurface surface;
    surface.parameters.base.color = {0.3, 0.5, 0.7};
    surface.image = world::AssetRef{valid_sha, "png"};
    const cs::Image image = three_by_two();
    Textures textures;
    textures.add(*surface.image, texture_of(image));
    const assets::Texture* texture = textures.find(*surface.image);
    REQUIRE(texture != nullptr);

    for (const dvec2 uv : {dvec2(0.5 / 3.0, 0.25), dvec2(2.5 / 3.0, 0.75), dvec2(0.5, 0.5),
                           dvec2(0.0, 0.0), dvec2(0.37, 0.81), dvec2(1.2, -0.1)}) {
        INFO("uv (" << uv.x << ", " << uv.y << ")");
        const dvec3 colour = painting_base_colour(surface, uv, &textures);
        check_colour(colour, sample_bilinear(*texture, uv));
        check_colour(colour, reference_bilinear(image, uv));
    }
}

TEST_CASE("painting_base_colour: returns the texel as the texture holds it, unclamped",
          "[engine][textures]") {
    // textures.hpp: "The value is the albedo as the texture holds it; the lux meter and path
    // tracer clamp it to [0, 1]".
    world::PaintingSurface surface;
    surface.image = world::AssetRef{valid_sha, "png"};
    cs::Image image(1, 1);
    set(image, 0, 0, {1.5, 0.5, 0.0});
    Textures textures;
    textures.add(*surface.image, texture_of(image));
    check_colour(painting_base_colour(surface, {0.5, 0.5}, &textures), {1.5, 0.5, 0.0});
}

// ---- asset_path --------------------------------------------------------------------------------

TEST_CASE("asset_path: <assets_dir>/<sha256>.<extension> for a valid name", "[engine][textures]") {
    const std::filesystem::path dir = "project/assets";
    for (const char* extension : {"png", "jpg", "tif", "exr"}) {
        INFO("extension " << extension);
        CHECK(asset_path(dir, world::AssetRef{valid_sha, extension}) ==
              dir / (valid_sha + "." + extension));
    }
    // Every hexadecimal digit is allowed.
    const std::string all_digits(64, 'f');
    CHECK(asset_path(dir, world::AssetRef{all_digits, "png"}) == dir / (all_digits + ".png"));
    CHECK(asset_path(dir, world::AssetRef{std::string(64, '0'), "jpg"}) ==
          dir / (std::string(64, '0') + ".jpg"));
}

TEST_CASE("asset_path: a hash that is not 64 lowercase hexadecimal digits throws "
          "std::invalid_argument",
          "[engine][textures]") {
    const std::filesystem::path dir = "project/assets";
    std::string uppercase = valid_sha;
    uppercase[10] = 'A'; // 'a' would be valid
    std::string embedded_nul = valid_sha;
    embedded_nul[20] = '\0';

    const std::vector<std::pair<std::string, std::string>> bad = {
        {"uppercase hexadecimal", uppercase},
        {"all uppercase", "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"},
        {"63 digits", valid_sha.substr(1)},
        {"65 digits", valid_sha + "0"},
        {"empty", ""},
        {"a letter past f", std::string(63, 'a') + "g"},
        {"a parent directory, 64 characters long", "../" + valid_sha.substr(3)},
        {"a parent directory, shorter", "../secret"},
        {"a path separator", valid_sha.substr(0, 32) + "/" + valid_sha.substr(33)},
        {"a backslash", valid_sha.substr(0, 32) + "\\" + valid_sha.substr(33)},
        {"an absolute path", "/" + valid_sha.substr(1)},
        {"a NUL character", embedded_nul},
        {"a space", " " + valid_sha.substr(1)},
    };
    for (const auto& [why, sha] : bad) {
        INFO(why);
        CHECK_THROWS_AS(asset_path(dir, world::AssetRef{sha, "png"}), std::invalid_argument);
    }
}

TEST_CASE("asset_path: an extension other than png, jpg, tif and exr throws std::invalid_argument",
          "[engine][textures]") {
    const std::filesystem::path dir = "project/assets";
    const std::vector<std::string> bad = {"",
                                          "PNG",
                                          "Png",
                                          "jpeg",
                                          "tiff",
                                          "gif",
                                          "../png",
                                          "png/..",
                                          "png/../../x",
                                          ".png",
                                          "png ",
                                          "exr\\..",
                                          std::string("png\0", 4)};
    for (const std::string& extension : bad) {
        INFO("extension '" << extension << "'");
        CHECK_THROWS_AS(asset_path(dir, world::AssetRef{valid_sha, extension}),
                        std::invalid_argument);
    }
}

// ---- load_textures -----------------------------------------------------------------------------

namespace {

using Bytes = std::vector<char>;

void append_bytes(void* context, void* data, int size) {
    auto* out = static_cast<Bytes*>(context);
    const auto* begin = static_cast<const char*>(data);
    out->insert(out->end(), begin, begin + size);
}

// An 8-bit RGB PNG of width x height pixels.
Bytes png8(int width, int height, const std::vector<std::uint8_t>& rgb) {
    Bytes out;
    REQUIRE(stbi_write_png_to_func(append_bytes, &out, width, height, 3, rgb.data(), width * 3) !=
            0);
    return out;
}

void write_file(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary);
    REQUIRE(file);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file);
}

// A directory of its own under the working directory, which ctest sets to this build's directory,
// so that other presets and worktrees running at the same time do not share it. Emptied when made
// and removed when the test ends.
class TempDir {
  public:
    explicit TempDir(const std::string& name)
        : path_(std::filesystem::current_path() / ("cs_engine_test_textures_" + name)) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

// A unit quad facing -y at the origin, with painting uvs, for worlds that only need instances.
geom::Mesh small_quad() {
    geom::Mesh mesh;
    mesh.positions = {{-0.5, 0.0, -0.5}, {0.5, 0.0, -0.5}, {0.5, 0.0, 0.5}, {-0.5, 0.0, 0.5}};
    mesh.normals.assign(4, dvec3(0.0, -1.0, 0.0));
    mesh.uvs = {{0.0, 1.0}, {1.0, 1.0}, {1.0, 0.0}, {0.0, 0.0}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

// A world with one painting instance per material given, in that order.
world::World paintings_world(const std::vector<world::Material>& materials) {
    world::World w;
    w.meshes.push_back(small_quad());
    for (const world::Material& m : materials) {
        world::Instance instance;
        instance.part = world::Part::painting;
        instance.mesh = 0;
        instance.material = static_cast<std::uint32_t>(w.materials.size());
        w.materials.push_back(m);
        w.instances.push_back(instance);
    }
    return w;
}

world::PaintingSurface painting_with(std::optional<world::AssetRef> image) {
    world::PaintingSurface surface;
    surface.image = std::move(image);
    return surface;
}

} // namespace

TEST_CASE("load_textures: a photograph two paintings name is loaded once, in linear light",
          "[engine][textures]") {
    const TempDir dir("once");
    const world::AssetRef ref{valid_sha, "png"};
    // 2 x 1 pixels: sRGB 188 grey and (255, 0, 51). assets::load_texture converts with the exact
    // sRGB curve, so the texture holds srgb_to_linear(v / 255).
    write_file(dir.path() / (valid_sha + ".png"), png8(2, 1, {188, 188, 188, 255, 0, 51}));

    const world::World w = paintings_world(
        {world::LambertMaterial{dvec3(0.5)}, painting_with(ref), painting_with(ref)});
    LoadedTextures loaded;
    REQUIRE_NOTHROW(loaded = load_textures(w, dir.path()));
    CHECK(loaded.problems.empty());
    CHECK(loaded.textures.size() == 1);

    const assets::Texture* texture = loaded.textures.find(ref);
    REQUIRE(texture != nullptr);
    CHECK(texture->width() == 2);
    CHECK(texture->height() == 1);
    const double grey = assets::srgb_to_linear(188.0 / 255.0);
    const double pink = assets::srgb_to_linear(51.0 / 255.0);
    check_colour(rgb_of(texture->levels[0].at(0, 0)), dvec3(grey));
    check_colour(rgb_of(texture->levels[0].at(1, 0)), {1.0, 0.0, pink});

    // And a painting reads it through painting_base_colour: u = 0.25 is texel 0's centre.
    check_colour(painting_base_colour(std::get<world::PaintingSurface>(w.materials[1]), {0.25, 0.5},
                                      &loaded.textures),
                 dvec3(grey));
}

TEST_CASE("load_textures: a world without photographs loads nothing and reports nothing",
          "[engine][textures]") {
    const TempDir dir("none");
    const world::World w =
        paintings_world({world::LambertMaterial{dvec3(0.5)}, painting_with(std::nullopt)});
    const LoadedTextures loaded = load_textures(w, dir.path());
    CHECK(loaded.textures.size() == 0);
    CHECK(loaded.problems.empty());
    CHECK(load_textures(world::World{}, dir.path()).problems.empty());
}

TEST_CASE("load_textures: a missing file, a file that is not an image and an invalid name are "
          "reported as problems, in the order of the materials, without throwing",
          "[engine][textures]") {
    const TempDir dir("problems");
    const world::AssetRef good{valid_sha, "png"};
    write_file(dir.path() / (valid_sha + ".png"), png8(1, 1, {10, 20, 30}));

    const world::AssetRef missing{other_sha, "png"};
    const std::string garbage_sha(64, 'c');
    const world::AssetRef garbage{garbage_sha, "png"};
    write_file(dir.path() / (garbage_sha + ".png"), Bytes{'n', 'o', 't', ' ', 'a', ' ', 'p', 'n'});
    // A name that would leave the folder; asset_path refuses it, and nothing is read.
    const world::AssetRef escaping{"../" + valid_sha.substr(3), "png"};
    std::string upper = valid_sha;
    upper[0] = 'F';
    const world::AssetRef uppercase{upper, "png"};

    const world::World w =
        paintings_world({painting_with(missing), painting_with(good), painting_with(escaping),
                         painting_with(garbage), painting_with(uppercase)});
    LoadedTextures loaded;
    REQUIRE_NOTHROW(loaded = load_textures(w, dir.path()));

    // The good one is loaded all the same.
    CHECK(loaded.textures.size() == 1);
    CHECK(loaded.textures.find(good) != nullptr);
    CHECK(loaded.textures.find(missing) == nullptr);
    CHECK(loaded.textures.find(garbage) == nullptr);
    CHECK(loaded.textures.find(escaping) == nullptr);
    CHECK(loaded.textures.find(uppercase) == nullptr);

    REQUIRE(loaded.problems.size() == 4);
    CHECK(loaded.problems[0].asset == missing);
    CHECK(loaded.problems[1].asset == escaping);
    CHECK(loaded.problems[2].asset == garbage);
    CHECK(loaded.problems[3].asset == uppercase);
    for (const cs::engine::TextureProblem& p : loaded.problems) {
        CHECK_FALSE(p.reason.empty());
    }

    // Their paintings show their base colour.
    const auto& surface = std::get<world::PaintingSurface>(w.materials[0]);
    check_colour(painting_base_colour(surface, {0.5, 0.5}, &loaded.textures),
                 surface.parameters.base.color);
}

TEST_CASE("load_textures: an assets folder that does not exist reports every photograph missing",
          "[engine][textures]") {
    const TempDir dir("absent");
    const world::World w = paintings_world({painting_with(world::AssetRef{valid_sha, "png"})});
    LoadedTextures loaded;
    REQUIRE_NOTHROW(loaded = load_textures(w, dir.path() / "no_such_folder"));
    CHECK(loaded.textures.size() == 0);
    REQUIRE_FALSE(loaded.problems.empty());
    for (const cs::engine::TextureProblem& p : loaded.problems) {
        CHECK(p.asset == world::AssetRef{valid_sha, "png"});
        CHECK_FALSE(p.reason.empty());
    }
}

// ---- The acceptance test: a 2 x 2 texture rendered face-on -------------------------------------

namespace {

// The four texels, all distinct and none the default 0.8 grey: red, green and blue at 0.2 to 0.8
// and a 0.5 grey. Texel (x, y), as images are stored: (0, 0) top-left, (1, 1) bottom-right.
const dvec3 top_left(0.8, 0.2, 0.2);
const dvec3 top_right(0.2, 0.8, 0.2);
const dvec3 bottom_left(0.2, 0.2, 0.8);
const dvec3 bottom_right(0.5, 0.5, 0.5);

cs::Image four_texels() {
    cs::Image image(2, 2);
    set(image, 0, 0, top_left);
    set(image, 1, 0, top_right);
    set(image, 0, 1, bottom_left);
    set(image, 1, 1, bottom_right);
    return image;
}

// A 1 x 1 m work centred at (0, 0, 1), facing -y, from geom::painting_quad: hung on a face whose
// u runs along +x and whose normal is -y (u x v = (1, 0, 0) x (0, 0, 1) = (0, -1, 0)), so seen
// from its front (from -y, looking +y) its right is +x and its top +z.
geom::Mesh quad_from_geom() {
    const geom::FaceFrame face{.origin = {-1.0, 0.0, 0.0},
                               .u = {1.0, 0.0, 0.0},
                               .v = {0.0, 0.0, 1.0},
                               .normal = {0.0, -1.0, 0.0},
                               .length_m = 2.0};
    return geom::painting_quad(face,
                               geom::FlatWork{.width_m = 1.0, .height_m = 1.0, .depth_m = 0.0},
                               geom::WallPlacement{.u_m = 1.0, .v_m = 1.0, .stand_off_m = 0.0});
}

// The same quad by hand, following painting.hpp's convention: the bottom-left, bottom-right,
// top-right and top-left corners seen from the front, with uv (0, 0) at the top-left and v down.
geom::Mesh quad_by_hand() {
    geom::Mesh mesh;
    mesh.positions = {{-0.5, 0.0, 0.5}, {0.5, 0.0, 0.5}, {0.5, 0.0, 1.5}, {-0.5, 0.0, 1.5}};
    mesh.normals.assign(4, dvec3(0.0, -1.0, 0.0));
    mesh.uvs = {{0.0, 1.0}, {1.0, 1.0}, {1.0, 0.0}, {0.0, 0.0}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

world::World one_painting(geom::Mesh mesh, std::optional<world::AssetRef> image) {
    world::World w;
    w.meshes.push_back(std::move(mesh));
    w.materials.emplace_back(painting_with(std::move(image)));
    world::Instance instance;
    instance.part = world::Part::painting;
    instance.mesh = 0;
    instance.material = 0;
    w.instances.push_back(instance);
    return w;
}

// An orthographic camera 2 m in front of the quad, looking at it face-on (+y) with +z up, its
// view exactly the quad's 1 m height. The image is square, so its width is the quad's 1 m too.
// The image's right is forward x up = (0, 1, 0) x (0, 0, 1) = (1, 0, 0) (path_tracer.hpp): the
// quad's right seen from the front. So pixel (x, y) of an n x n image sees uv
// ((x + 0.5) / n, (y + 0.5) / n), up to the samples' spread over the pixel.
world::Camera face_on_camera() {
    world::Camera camera;
    camera.projection = world::Projection::orthographic;
    camera.position_m = {0.0, -2.0, 1.0};
    camera.forward = {0.0, 1.0, 0.0};
    camera.up = {0.0, 0.0, 1.0};
    camera.vertical_extent_m = 1.0;
    return camera;
}

constexpr int image_side = 32;

cs::engine::RenderOptions face_on_options(const Textures* textures) {
    // A uniform white environment of 1 nit and no lights: a Lambert surface alone in it reflects
    // exactly its albedo times the environment (the integral of rho / pi x 1 x cos over the
    // hemisphere is rho), and every path leaves after one bounce, the quad being flat. direct_only
    // keeps exactly that light (path_tracer.hpp). So each pixel shows, in nits, the albedo the
    // engine read at the uv its samples hit.
    cs::engine::RenderOptions o;
    o.width = image_side;
    o.height = image_side;
    o.samples_per_pixel = 64;
    o.seed = 1;
    o.direct_only = true;
    o.environment_nits = 1.0;
    o.textures = textures;
    return o;
}

// Each path weighs the environment by the albedo exactly, up to the float rounding of the BRDF's
// weight and of the image (about 1e-6, as test_path_tracer.cpp finds for a Lambert floor); 1e-5
// allows that and nothing else.
constexpr double exact_pixel_tolerance = 1e-5;

void check_pixel(const cs::Image& image, int x, int y, dvec3 expected, double tolerance) {
    const cs::Rgb& p = image.at(x, y);
    INFO("pixel (" << x << ", " << y << ") is (" << p.r << ", " << p.g << ", " << p.b
                   << "), expected (" << expected.x << ", " << expected.y << ", " << expected.z
                   << ")");
    CHECK_THAT(p.r, WithinRel(expected.x, tolerance));
    CHECK_THAT(p.g, WithinRel(expected.y, tolerance));
    CHECK_THAT(p.b, WithinRel(expected.z, tolerance));
}

} // namespace

TEST_CASE("render: a 2 x 2-texel texture rendered face-on shows each texel's colour in its "
          "quadrant, unmirrored",
          "[engine][textures][path_tracer][accuracy]") {
    // The issue's criterion. The texels' centres are at uv 0.25 and 0.75 (textures.hpp), so with
    // clamp to edge everything with u < 0.25 and v < 0.25 reads the top-left texel alone, and so
    // on for each corner: pixels 0 to 7 of each side of the 32 x 32 image (pixel 7 ends at
    // u = 8 / 32 = 0.25). Pixels 1 to 6 are checked: pixel 0 is left out because samples at the
    // very edge of the view could just miss the quad in Embree's single precision and see the
    // 1-nit environment, pixel 7 to stay clear of the first texel centre. Every one of their
    // samples reads the texel exactly, so the pixel is the texel's colour to rounding.
    //
    // Seen from the front, the image's top-left is the texture's top-left: a mirrored texture (u
    // or v reversed, or uvs taken as v up) puts another texel there and fails.
    const bool by_hand = GENERATE(false, true);
    INFO((by_hand ? "a quad made by hand" : "geom::painting_quad"));

    const world::AssetRef ref{valid_sha, "png"};
    Textures textures;
    textures.add(ref, texture_of(four_texels()));
    const cs::engine::Scene scene(one_painting(by_hand ? quad_by_hand() : quad_from_geom(), ref));
    const cs::Image image = cs::engine::render(scene, face_on_camera(), face_on_options(&textures));
    REQUIRE(image.width() == image_side);
    REQUIRE(image.height() == image_side);

    // The texels as stored (floats), as the engine reads them.
    const cs::Image texels = four_texels();
    struct Quadrant {
        const char* name;
        int x0;
        int y0;
        dvec3 colour;
    };
    for (const Quadrant& q : {Quadrant{"top-left", 1, 1, rgb_of(texels.at(0, 0))},
                              Quadrant{"top-right", 25, 1, rgb_of(texels.at(1, 0))},
                              Quadrant{"bottom-left", 1, 25, rgb_of(texels.at(0, 1))},
                              Quadrant{"bottom-right", 25, 25, rgb_of(texels.at(1, 1))}}) {
        INFO(q.name << " quadrant");
        for (int y = q.y0; y < q.y0 + 6; ++y) {
            for (int x = q.x0; x < q.x0 + 6; ++x) {
                check_pixel(image, x, y, q.colour, exact_pixel_tolerance);
            }
        }
    }

    // The four pixels round the image's centre blend all four texels. Over pixel 15 the weight of
    // texel column 1 is 2u - 0.5 (0 at u = 0.25, 1 at u = 0.75), whose mean over u in
    // [15/32, 16/32] is 2 x 15.5 / 32 - 0.5 = 0.46875; over pixel 16, 0.53125. Rows likewise. The
    // samples spread uniformly and independently in x and y over the pixel, so the pixel's
    // expected colour is the four texels weighted by products of those means. Within a pixel the
    // weights vary by 1/16, so one sample's colour spreads by about 0.6 x 0.0625 / sqrt(12) =
    // 0.011 per direction; 64 samples bring the pixel's error to about 0.002: 0.01 is 5 of those.
    const auto mean_weight = [](int pixel) { return 2.0 * (pixel + 0.5) / image_side - 0.5; };
    for (const int y : {15, 16}) {
        for (const int x : {15, 16}) {
            const double wx = mean_weight(x);
            const double wy = mean_weight(y);
            const dvec3 expected = (1.0 - wx) * (1.0 - wy) * rgb_of(texels.at(0, 0)) +
                                   wx * (1.0 - wy) * rgb_of(texels.at(1, 0)) +
                                   (1.0 - wx) * wy * rgb_of(texels.at(0, 1)) +
                                   wx * wy * rgb_of(texels.at(1, 1));
            const cs::Rgb& p = image.at(x, y);
            INFO("centre pixel (" << x << ", " << y << ") is (" << p.r << ", " << p.g << ", " << p.b
                                  << ")");
            CHECK_THAT(p.r, WithinAbs(expected.x, 0.01));
            CHECK_THAT(p.g, WithinAbs(expected.y, 0.01));
            CHECK_THAT(p.b, WithinAbs(expected.z, 0.01));
        }
    }
}

TEST_CASE("render: without textures, or without the painting's, the face shows its base colour",
          "[engine][textures][path_tracer]") {
    // The same view: every pixel inside the edge row shows the default base colour, 0.8 grey,
    // times the 1-nit environment.
    const world::AssetRef ref{valid_sha, "png"};
    Textures others;
    others.add(world::AssetRef{other_sha, "png"}, texture_of(four_texels()));
    const cs::engine::Scene scene(one_painting(quad_from_geom(), ref));

    const Textures* none = nullptr;
    const Textures* other_textures = &others;
    for (const Textures* textures : {none, other_textures}) {
        INFO((textures == nullptr ? "no textures" : "textures without the painting's"));
        const cs::Image image =
            cs::engine::render(scene, face_on_camera(), face_on_options(textures));
        for (int y = 1; y < image_side - 1; ++y) {
            for (int x = 1; x < image_side - 1; ++x) {
                check_pixel(image, x, y, dvec3(0.8), exact_pixel_tolerance);
            }
        }
    }
}

namespace {

// The quad of quad_from_geom lit by a wide spot in front of it, without a room: a sensor below
// the quad, facing up, sees the quad and so reads its reflected light as indirect.
world::World lit_painting(const world::PaintingSurface& surface) {
    world::World w = one_painting(quad_from_geom(), std::nullopt);
    w.materials[0] = surface;
    auto spot = std::make_shared<const cs::light::GenericSpot>(1000.0, 1.0, 1.5);
    world::Light light{world::Id{}, cs::light::Luminaire({0.0, -1.0, 2.0}, {0.0, 0.0, 1.0}, spot),
                       spot, 3000.0};
    w.lights.push_back(std::move(light));
    return w;
}

} // namespace

TEST_CASE("lux_at and render: a uniform photograph of colour c reflects exactly as base colour c",
          "[engine][textures][lux][path_tracer]") {
    // The texture holds c in floats; c is exact in float, so both paintings have the same albedo
    // bit for bit and, with the same seeds, every path draws the same numbers: the readings and
    // the images must be equal exactly, not within a tolerance. A lux meter that ignored the
    // texture would read the default 0.8 grey instead, and differ.
    const dvec3 c(0.25, 0.5, 0.75);
    const world::AssetRef ref{valid_sha, "png"};
    cs::Image one(1, 1);
    set(one, 0, 0, c);
    Textures textures;
    textures.add(ref, texture_of(one));

    world::PaintingSurface photographed = painting_with(ref);
    world::PaintingSurface plain = painting_with(std::nullopt);
    plain.parameters.base.color = c;
    const cs::engine::Scene with_photograph(lit_painting(photographed));
    const cs::engine::Scene with_colour(lit_painting(plain));

    cs::engine::LuxOptions lux;
    lux.samples = 256;
    lux.textures = &textures;
    const dvec3 point(0.0, -0.5, 0.0);
    const dvec3 up(0.0, 0.0, 1.0);
    const cs::engine::LuxReading a = cs::engine::lux_at(with_photograph, point, up, lux);
    const cs::engine::LuxReading b = cs::engine::lux_at(with_colour, point, up, lux);
    REQUIRE(a.total_lux.value > a.direct_lux.value); // the quad's light reaches the sensor
    CHECK(a.direct_lux.value == b.direct_lux.value);
    CHECK(a.total_lux.value == b.total_lux.value);
    CHECK(a.total_lux.standard_error == b.total_lux.standard_error);

    world::Camera camera;
    camera.position_m = {0.0, -2.0, 1.0};
    camera.forward = {0.0, 1.0, 0.0};
    camera.up = {0.0, 0.0, 1.0};
    camera.vertical_fov_rad = 0.6;
    cs::engine::RenderOptions render;
    render.width = 16;
    render.height = 16;
    render.samples_per_pixel = 8;
    render.textures = &textures;
    const cs::Image image_a = cs::engine::render(with_photograph, camera, render);
    const cs::Image image_b = cs::engine::render(with_colour, camera, render);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            CHECK(rgb_of(image_a.at(x, y)) == rgb_of(image_b.at(x, y)));
        }
    }
}
