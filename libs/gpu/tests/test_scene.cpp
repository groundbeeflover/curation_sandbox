// SPDX-License-Identifier: Apache-2.0

// The render world on the GPU (scene.hpp, issue M1-21): meshes, instances, materials and the
// paintings' textures with their full mip chains, uploaded once and read back.
//
// The gpu module may not depend on the document (blueprint section 7), so the worlds here are
// built by hand; the real skeleton project, loaded and compiled, is uploaded in
// tests/integration/test_skeleton_gpu_scene.cpp. The skeleton's chart is used here too, through
// project_texture_loader, in a hand-made world.
//
// Every test that needs a device starts with make_device() (gpu_test.hpp): without a Vulkan
// device it skips, and with CS_REQUIRE_GPU=1 it fails. The harness (main.cpp) fails the running
// test on any validation warning or error, so every test here is also criterion 3's "validation
// clean". Scenes are declared after the device, so they are destroyed before it, also when an
// assertion throws.
//
// Criterion 1's "uploaded with one submission" is not visible through the public interface and
// is not tested here; that the upload is complete when the constructor returns is (every
// download follows it directly).

#include "gpu_test.hpp"

#include <cs/assets/texture.hpp>
#include <cs/core/image.hpp>
#include <cs/geom/mesh.hpp>
#include <cs/gpu/device.hpp>
#include <cs/gpu/scene.hpp>
#include <cs/material/painting_material.hpp>
#include <cs/world/ids.hpp>
#include <cs/world/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using cs::gpu::Device;
using cs::gpu::InstanceData;
using cs::gpu::MaterialData;
using cs::gpu::MaterialKind;
using cs::gpu::MemoryInUse;
using cs::gpu::MeshRange;
using cs::gpu::no_texture;
using cs::gpu::Scene;
using cs::gpu::SceneBuffer;
using cs::gpu::SceneBufferKind;
using cs::gpu::TextureLoader;
using cs::gpu::Vertex;
using cs::gpu::test::make_device;
using cs::world::AssetRef;

namespace {

// ---- Tolerances --------------------------------------------------------------------------------

// Textures are uploaded as half floats (scene.hpp). Half floats have 11 significant bits, so in
// [0.5, 1) their spacing is 2^-11 = 4.9e-4, and below 0.5 it is smaller still: converting a
// linear value in [0, 1] to half and back is off by at most 4.9e-4 even if the conversion
// truncated, and by half that when it rounds to nearest. 1e-3 is the issue's criterion 2, twice
// that worst case; a texel from the wrong place, a flipped row or a missed sRGB step is off by far
// more.
constexpr float texel_tolerance = 1e-3f;

// ---- Assets ------------------------------------------------------------------------------------

// The skeleton scene's chart, in the repository (tests/scenes/skeleton/README.md).
constexpr std::string_view chart_sha256 =
    "26d3b28857e3da2b25625116fcf14d505ad4ff83944c83cdb74759ad56f23b6a";

std::filesystem::path skeleton_assets() {
    return std::filesystem::path(CS_SOURCE_DIR) / "tests" / "scenes" / "skeleton" / "assets";
}

std::filesystem::path chart_path() {
    return skeleton_assets() / (std::string(chart_sha256) + ".png");
}

AssetRef chart_asset() {
    return AssetRef{std::string(chart_sha256), "png"};
}

// Made-up assets for hand-made worlds: the loaders below decide what they hold.
AssetRef asset(char digit, std::string extension = "png") {
    return AssetRef{std::string(64, digit), std::move(extension)};
}

// A linear image whose every value is different and in (0, 1), so that a texel read from the
// wrong place, a flipped row or swapped channels all show: value k of the image (in row-major
// order, three per pixel) is (k + 1) / (3 w h + 1).
cs::Image graded_image(int width, int height) {
    cs::Image image(width, height);
    const float n = static_cast<float>(3 * width * height + 1);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int k = 3 * (y * width + x);
            image.at(x, y) = cs::Rgb{static_cast<float>(k + 1) / n, static_cast<float>(k + 2) / n,
                                     static_cast<float>(k + 3) / n};
        }
    }
    return image;
}

cs::assets::Texture graded_texture(int width, int height) {
    return cs::assets::Texture{cs::assets::build_mip_chain(graded_image(width, height))};
}

// A loader that gives the textures of a map, keyed by sha256, and throws std::runtime_error, as
// TextureLoader's contract says, for any other asset. The map must outlive the loader.
TextureLoader loader_from(const std::map<std::string, cs::assets::Texture>& textures) {
    return [&textures](const AssetRef& ref) -> cs::assets::Texture {
        const auto found = textures.find(ref.sha256);
        if (found == textures.end()) {
            throw std::runtime_error("test loader: no texture for " + ref.sha256);
        }
        return found->second;
    };
}

// A loader for worlds without images: any call is a failure of the test.
TextureLoader no_loader() {
    return [](const AssetRef& ref) -> cs::assets::Texture {
        FAIL_CHECK("the loader was called for " << ref.sha256 << ", in a world without images");
        throw std::runtime_error("unexpected load");
    };
}

// The largest difference of any channel of any pixel; the sizes must match.
float max_abs_difference(const cs::Image& a, const cs::Image& b) {
    REQUIRE(a.width() == b.width());
    REQUIRE(a.height() == b.height());
    float worst = 0.0f;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const cs::Rgb& p = a.at(x, y);
            const cs::Rgb& q = b.at(x, y);
            worst =
                std::max({worst, std::abs(p.r - q.r), std::abs(p.g - q.g), std::abs(p.b - q.b)});
        }
    }
    return worst;
}

// ---- Meshes and worlds -------------------------------------------------------------------------

// A 0.73 x 0.92 m quad facing -y, two triangles. 0.73 and 0.92 are not floats, so the
// conversion's rounding is part of what is compared.
cs::geom::Mesh quad_mesh() {
    cs::geom::Mesh mesh;
    mesh.positions = {{0.0, 0.0, 0.0}, {0.73, 0.0, 0.0}, {0.73, 0.0, 0.92}, {0.0, 0.0, 0.92}};
    mesh.normals.assign(4, glm::dvec3(0.0, -1.0, 0.0));
    mesh.uvs = {{0.0, 0.0}, {0.73, 0.0}, {0.73, 0.92}, {0.0, 0.92}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

// One triangle with a tilted normal, (0.6, 0, 0.8), unit but not exact in float.
cs::geom::Mesh triangle_mesh() {
    cs::geom::Mesh mesh;
    mesh.positions = {{0.1, 0.2, 0.3}, {1.1, 0.2, 0.3}, {0.1, 1.3, 0.3}};
    mesh.normals.assign(3, glm::dvec3(0.6, 0.0, 0.8));
    mesh.uvs = {{0.1, 0.2}, {1.1, 0.2}, {0.1, 1.3}};
    mesh.indices = {0, 1, 2};
    return mesh;
}

cs::world::Instance instance(std::uint32_t mesh, std::uint32_t material,
                             const glm::dmat4& transform = glm::dmat4(1.0)) {
    cs::world::Instance result;
    result.mesh = mesh;
    result.material = material;
    result.transform = transform;
    return result;
}

cs::world::PaintingSurface painting(std::optional<AssetRef> image,
                                    glm::dvec3 colour = glm::dvec3(0.8)) {
    cs::world::PaintingSurface surface;
    surface.parameters.base.color = colour;
    surface.image = std::move(image);
    return surface;
}

// A rotation about z by 0.3 rad and a translation: cos 0.3 and sin 0.3 are not floats.
glm::dmat4 placed(glm::dvec3 translation_m) {
    return glm::rotate(glm::translate(glm::dmat4(1.0), translation_m), 0.3,
                       glm::dvec3(0.0, 0.0, 1.0));
}

// The world of the main test: two meshes, three instances, four materials.
//   materials: 0 Lambert, 1 a painting showing asset 'a', 2 a second painting showing the same
//              asset, 3 a painting with no image
//   instances: 0 the triangle in Lambert, 1 the quad as painting 1, 2 the quad as painting 2
// Material 3 is used by no instance; it is in the materials buffer all the same, as the buffer
// follows World::materials.
cs::world::World small_world() {
    cs::world::World world;
    world.meshes = {quad_mesh(), triangle_mesh()};

    world.materials.emplace_back(cs::world::LambertMaterial{glm::dvec3(0.25, 0.5, 0.7)});

    cs::world::PaintingSurface varnished = painting(asset('a'), glm::dvec3(0.3, 0.4, 0.55));
    varnished.parameters.base.roughness = 0.45;
    varnished.parameters.base.specular_weight = 0.0; // under a coat (painting_material.hpp)
    varnished.parameters.coat.weight = 0.7;
    varnished.parameters.coat.roughness = 0.18;
    world.materials.emplace_back(varnished);

    world.materials.emplace_back(painting(asset('a'), glm::dvec3(0.6, 0.6, 0.6)));
    world.materials.emplace_back(painting(std::nullopt, glm::dvec3(0.1, 0.2, 0.3)));

    world.instances = {
        instance(1, 0, placed({1.0, 2.0, 0.5})),
        instance(0, 1, placed({3.0, 0.04, 1.5})),
        instance(0, 2, glm::translate(glm::dmat4(1.0), glm::dvec3(-0.1, 3.9, 1.2))),
    };
    return world;
}

// The world's vertices as the scene must hold them: mesh after mesh, each double rounded to the
// nearest float (static_cast does so, in the default rounding mode).
std::vector<Vertex> expected_vertices(const cs::world::World& world) {
    std::vector<Vertex> vertices;
    for (const cs::geom::Mesh& mesh : world.meshes) {
        for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
            Vertex v;
            v.position_m = glm::vec3(mesh.positions[i]);
            v.normal = glm::vec3(mesh.normals[i]);
            v.uv = glm::vec2(mesh.uvs[i]);
            vertices.push_back(v);
        }
    }
    return vertices;
}

// The indices as the scene must hold them: still counted from each mesh's own first vertex.
std::vector<std::uint32_t> expected_indices(const cs::world::World& world) {
    std::vector<std::uint32_t> indices;
    for (const cs::geom::Mesh& mesh : world.meshes) {
        indices.insert(indices.end(), mesh.indices.begin(), mesh.indices.end());
    }
    return indices;
}

// ---- Reading buffers ---------------------------------------------------------------------------

template <typename T> std::vector<T> elements_of(const std::vector<std::byte>& bytes) {
    REQUIRE(bytes.size() % sizeof(T) == 0);
    std::vector<T> elements(bytes.size() / sizeof(T));
    if (!bytes.empty()) {
        std::memcpy(elements.data(), bytes.data(), bytes.size());
    }
    return elements;
}

template <typename T>
bool same_bytes(const std::vector<std::byte>& downloaded, std::span<const T> host) {
    const std::span<const std::byte> bytes = std::as_bytes(host);
    return std::equal(downloaded.begin(), downloaded.end(), bytes.begin(), bytes.end());
}

// Exact on purpose: both sides are the same double rounded to float once.
bool same_vertex(const Vertex& a, const Vertex& b) {
    return a.position_m == b.position_m && a.normal == b.normal && a.uv == b.uv;
}

bool same_transform(const glm::mat4& a, const glm::dmat4& b) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (a[column][row] != static_cast<float>(b[column][row])) {
                return false;
            }
        }
    }
    return true;
}

// ---- Files -------------------------------------------------------------------------------------

// The folder `gpu_test_scratch/<name>` under the working directory, which CTest sets to the
// module's build folder, removed before and after. The name is a fixed literal of the test's.
class TempFolder {
  public:
    explicit TempFolder(std::string_view name)
        : path_(std::filesystem::current_path() / "gpu_test_scratch" / std::string(name)) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;

    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

VkPhysicalDeviceLimits limits_of(const Device& device) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.physical_device(), &properties);
    return properties.limits;
}

} // namespace

// ---- Criterion 1: vertex and index buffers ------------------------------------------------------

TEST_CASE("scene: the vertex buffer holds the world's vertices as floats, mesh after mesh, and the "
          "index buffer their mesh-local indices",
          "[gpu][scene]") {
    const std::unique_ptr<Device> gpu = make_device();
    const cs::world::World world = small_world();
    const std::map<std::string, cs::assets::Texture> textures{
        {asset('a').sha256, graded_texture(5, 3)}};
    const Scene scene(*gpu, world, loader_from(textures));
    CHECK(scene.problems().empty());

    const std::vector<Vertex> expected = expected_vertices(world);
    REQUIRE(expected.size() == 7); // 4 of the quad, 3 of the triangle

    SECTION("vertices: exactly the world's, rounded to float, in order") {
        CHECK(scene.vertices().buffer != VK_NULL_HANDLE);
        CHECK(scene.vertices().address != 0);
        CHECK(scene.vertices().size_bytes == expected.size() * sizeof(Vertex));

        const std::vector<Vertex> downloaded =
            elements_of<Vertex>(scene.download(SceneBufferKind::vertices));
        REQUIRE(downloaded.size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            INFO("vertex " << i);
            CHECK(same_vertex(downloaded[i], expected[i]));
        }
    }

    SECTION("indices: the meshes' own, not offset by the mesh's first vertex") {
        const std::vector<std::uint32_t> expected_index = expected_indices(world);
        REQUIRE(expected_index == std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3, 0, 1, 2});
        CHECK(scene.indices().buffer != VK_NULL_HANDLE);
        CHECK(scene.indices().address != 0);
        CHECK(scene.indices().size_bytes == expected_index.size() * sizeof(std::uint32_t));
        CHECK(elements_of<std::uint32_t>(scene.download(SceneBufferKind::indices)) ==
              expected_index);
    }

    SECTION("meshes(): where each mesh lies, in elements") {
        const std::span<const MeshRange> meshes = scene.meshes();
        REQUIRE(meshes.size() == 2);
        CHECK(meshes[0].first_vertex == 0);
        CHECK(meshes[0].vertex_count == 4);
        CHECK(meshes[0].first_index == 0);
        CHECK(meshes[0].index_count == 6);
        CHECK(meshes[1].first_vertex == 4);
        CHECK(meshes[1].vertex_count == 3);
        CHECK(meshes[1].first_index == 6);
        CHECK(meshes[1].index_count == 3);
    }
}

// ---- Criterion 1: per-instance transforms and material indices ----------------------------------

TEST_CASE("scene: one InstanceData per instance, with its transform as floats, its mesh, its "
          "material and its mesh's range",
          "[gpu][scene]") {
    const std::unique_ptr<Device> gpu = make_device();
    const cs::world::World world = small_world();
    const std::map<std::string, cs::assets::Texture> textures{
        {asset('a').sha256, graded_texture(5, 3)}};
    const Scene scene(*gpu, world, loader_from(textures));

    // From the layout of small_world(): the quad (mesh 0) has vertices 0..3 and indices 0..5, the
    // triangle (mesh 1) vertices 4..6 and indices 6..8.
    const std::uint32_t first_index_of[2] = {0, 6};
    const std::uint32_t vertex_offset_of[2] = {0, 4};

    const std::span<const InstanceData> instances = scene.instance_data();
    REQUIRE(instances.size() == world.instances.size());
    for (std::size_t i = 0; i < instances.size(); ++i) {
        INFO("instance " << i);
        const cs::world::Instance& source = world.instances[i];
        CHECK(same_transform(instances[i].transform, source.transform));
        CHECK(instances[i].mesh == source.mesh);
        CHECK(instances[i].material == source.material);
        CHECK(instances[i].first_index == first_index_of[source.mesh]);
        CHECK(instances[i].vertex_offset == vertex_offset_of[source.mesh]);
    }

    // The GPU's copy is the host's, byte for byte (InstanceData has no padding: 64 + 4 x 4).
    CHECK(scene.instances().buffer != VK_NULL_HANDLE);
    CHECK(scene.instances().address != 0);
    CHECK(scene.instances().size_bytes == instances.size() * sizeof(InstanceData));
    const std::vector<std::byte> downloaded = scene.download(SceneBufferKind::instances);
    REQUIRE(downloaded.size() == instances.size() * sizeof(InstanceData));
    CHECK(same_bytes(downloaded, instances));
}

// ---- Criterion 1: the materials buffer ----------------------------------------------------------

TEST_CASE("scene: one MaterialData per material, Lambert or painting, with its colour and texture; "
          "a shared asset is one texture",
          "[gpu][scene]") {
    const std::unique_ptr<Device> gpu = make_device();
    const cs::world::World world = small_world();
    const std::map<std::string, cs::assets::Texture> textures{
        {asset('a').sha256, graded_texture(5, 3)}};
    const Scene scene(*gpu, world, loader_from(textures));
    CHECK(scene.problems().empty());

    const std::span<const MaterialData> materials = scene.material_data();
    REQUIRE(materials.size() == 4);

    // Materials 1 and 2 show the same asset: one texture, which both name.
    REQUIRE(scene.textures().size() == 1);
    CHECK(scene.textures()[0].asset == asset('a'));

    SECTION("Lambert: its albedo, no texture") {
        CHECK(materials[0].kind == MaterialKind::lambert);
        CHECK(materials[0].base_colour == glm::vec3(glm::dvec3(0.25, 0.5, 0.7)));
        CHECK(materials[0].texture == no_texture);
    }
    SECTION("a painting with an image: its PaintingMaterial's base and coat, and the texture") {
        const auto& source = std::get<cs::world::PaintingSurface>(world.materials[1]).parameters;
        CHECK(materials[1].kind == MaterialKind::painting);
        CHECK(materials[1].texture == 0);
        CHECK(materials[1].base_colour == glm::vec3(source.base.color));
        CHECK(materials[1].roughness == static_cast<float>(source.base.roughness));
        CHECK(materials[1].specular_weight == static_cast<float>(source.base.specular_weight));
        CHECK(materials[1].coat_weight == static_cast<float>(source.coat.weight));
        CHECK(materials[1].coat_roughness == static_cast<float>(source.coat.roughness));
        // The material's own (1.52); the rasterised tier substitutes glTF's 1.5 (scene.hpp).
        CHECK(materials[1].coat_ior == static_cast<float>(source.coat.ior));
    }
    SECTION("the second painting of the same asset names the same texture") {
        CHECK(materials[2].kind == MaterialKind::painting);
        CHECK(materials[2].texture == 0);
        CHECK(materials[2].base_colour == glm::vec3(glm::dvec3(0.6)));
    }
    SECTION("a painting without an image: its base colour, no texture") {
        CHECK(materials[3].kind == MaterialKind::painting);
        CHECK(materials[3].texture == no_texture);
        CHECK(materials[3].base_colour == glm::vec3(glm::dvec3(0.1, 0.2, 0.3)));
    }
    SECTION("the GPU's copy is the host's, byte for byte") {
        CHECK(scene.materials().buffer != VK_NULL_HANDLE);
        CHECK(scene.materials().address != 0);
        CHECK(scene.materials().size_bytes == materials.size() * sizeof(MaterialData));
        const std::vector<std::byte> downloaded = scene.download(SceneBufferKind::materials);
        REQUIRE(downloaded.size() == materials.size() * sizeof(MaterialData));
        CHECK(same_bytes(downloaded, materials));
    }
}

// ---- Criterion 1: textures with full mip chains
// --------------------------------------------------

TEST_CASE("scene: every mip level of a texture reads back as the asset's level within 1e-3, and "
          "the chain is complete",
          "[gpu][scene][accuracy]") {
    const std::unique_ptr<Device> gpu = make_device();

    // Three paintings of odd and extreme shapes. Their chains, halving and rounding down to 1:
    //   5 x 3: 5x3, 2x1, 1x1        3 levels
    //   7 x 1: 7x1, 3x1, 1x1        3 levels
    //   1 x 1: 1x1                  1 level
    struct Case {
        char digit;
        int width;
        int height;
        std::uint32_t levels;
    };
    const Case cases[] = {{'a', 5, 3, 3}, {'b', 7, 1, 3}, {'c', 1, 1, 1}};

    std::map<std::string, cs::assets::Texture> textures;
    cs::world::World world;
    world.meshes = {quad_mesh()};
    for (const Case& c : cases) {
        textures.emplace(asset(c.digit).sha256, graded_texture(c.width, c.height));
        REQUIRE(textures.at(asset(c.digit).sha256).levels.size() == c.levels);
        world.materials.emplace_back(painting(asset(c.digit)));
        world.instances.push_back(
            instance(0, static_cast<std::uint32_t>(world.materials.size() - 1)));
    }

    const Scene scene(*gpu, world, loader_from(textures));
    CHECK(scene.problems().empty());
    REQUIRE(scene.textures().size() == 3);

    for (std::size_t m = 0; m < 3; ++m) {
        const Case& c = cases[m];
        INFO(c.width << " x " << c.height);
        const std::uint32_t index = scene.material_data()[m].texture;
        REQUIRE(index < scene.textures().size());
        const cs::gpu::Texture& texture = scene.textures()[index];
        CHECK(texture.asset == asset(c.digit));
        CHECK(texture.width == static_cast<std::uint32_t>(c.width));
        CHECK(texture.height == static_cast<std::uint32_t>(c.height));
        CHECK(texture.mip_levels == c.levels);
        CHECK(texture.image != VK_NULL_HANDLE);
        CHECK(texture.view != VK_NULL_HANDLE);

        const cs::assets::Texture& source = textures.at(asset(c.digit).sha256);
        for (std::uint32_t level = 0; level < c.levels; ++level) {
            INFO("level " << level);
            const cs::Image downloaded = scene.download_texture(index, level);
            CHECK(max_abs_difference(downloaded, source.levels[level]) <= texel_tolerance);
        }
        CHECK_THROWS_AS(static_cast<void>(scene.download_texture(index, c.levels)),
                        std::out_of_range);
    }
    CHECK_THROWS_AS(static_cast<void>(scene.download_texture(3, 0)), std::out_of_range);
    CHECK_THROWS_AS(static_cast<void>(scene.download_texture(no_texture, 0)), std::out_of_range);
}

// ---- Criterion 2, in the gpu module: the skeleton's chart through the project loader ------------

TEST_CASE("scene: the skeleton's chart, loaded by project_texture_loader, reads back as "
          "assets::load_texture's image within 1e-3",
          "[gpu][scene][accuracy]") {
    const std::unique_ptr<Device> gpu = make_device();

    cs::world::World world;
    world.meshes = {quad_mesh()};
    world.materials.emplace_back(painting(chart_asset()));
    world.instances = {instance(0, 0)};

    const Scene scene(*gpu, world, cs::gpu::project_texture_loader(skeleton_assets()));
    for (const cs::gpu::TextureProblem& problem : scene.problems()) {
        UNSCOPED_INFO("problem: " << problem.reason);
    }
    REQUIRE(scene.problems().empty());
    REQUIRE(scene.textures().size() == 1);
    REQUIRE(scene.material_data()[0].texture == 0);

    const cs::assets::Texture expected = cs::assets::load_texture(chart_path());
    const cs::gpu::Texture& texture = scene.textures()[0];
    CHECK(texture.asset == chart_asset());
    CHECK(texture.width == static_cast<std::uint32_t>(expected.width()));
    CHECK(texture.height == static_cast<std::uint32_t>(expected.height()));
    REQUIRE(texture.mip_levels == expected.levels.size());

    // Mip 0 is the criterion; the other levels are checked too, as they cost little.
    for (std::uint32_t level = 0; level < texture.mip_levels; ++level) {
        INFO("level " << level);
        const cs::Image downloaded = scene.download_texture(0, level);
        CHECK(max_abs_difference(downloaded, expected.levels[level]) <= texel_tolerance);
    }
}

// ---- Textures that cannot be uploaded are problems, not errors
// -----------------------------------

TEST_CASE("scene: an asset the loader cannot give is a TextureProblem naming it, and its "
          "materials show their base colour",
          "[gpu][scene]") {
    const std::unique_ptr<Device> gpu = make_device();

    SECTION("one asset fails, another loads") {
        cs::world::World world;
        world.meshes = {quad_mesh()};
        world.materials.emplace_back(painting(asset('a'), glm::dvec3(0.3, 0.4, 0.5)));
        world.materials.emplace_back(painting(asset('b')));
        world.materials.emplace_back(painting(asset('a'))); // shares the failing asset
        world.instances = {instance(0, 0), instance(0, 1), instance(0, 2)};

        const cs::assets::Texture good = graded_texture(5, 3);
        const TextureLoader loader = [&good](const AssetRef& ref) -> cs::assets::Texture {
            if (ref == asset('a')) {
                throw std::runtime_error("deliberately unreadable asset");
            }
            return good;
        };
        const Scene scene(*gpu, world, loader);

        // One problem for the asset, however many materials show it.
        REQUIRE(scene.problems().size() == 1);
        CHECK(scene.problems()[0].asset == asset('a'));
        CHECK_FALSE(scene.problems()[0].reason.empty());
        // "says why": the loader's message is assumed to be part of the reason.
        CHECK_THAT(scene.problems()[0].reason, ContainsSubstring("deliberately unreadable asset"));

        REQUIRE(scene.textures().size() == 1);
        CHECK(scene.textures()[0].asset == asset('b'));

        const std::span<const MaterialData> materials = scene.material_data();
        REQUIRE(materials.size() == 3);
        CHECK(materials[0].kind == MaterialKind::painting);
        CHECK(materials[0].texture == no_texture);
        CHECK(materials[0].base_colour == glm::vec3(glm::dvec3(0.3, 0.4, 0.5)));
        CHECK(materials[1].texture == 0);
        CHECK(materials[2].texture == no_texture);
        CHECK(max_abs_difference(scene.download_texture(0, 0), good.levels[0]) <= texel_tolerance);
    }

    SECTION("every load fails: the scene still builds, with no textures") {
        const cs::world::World world = small_world();
        const TextureLoader loader = [](const AssetRef&) -> cs::assets::Texture {
            throw std::runtime_error("deliberately unreadable asset");
        };
        const Scene scene(*gpu, world, loader);
        REQUIRE(scene.problems().size() == 1);
        CHECK(scene.problems()[0].asset == asset('a'));
        CHECK(scene.textures().empty());
        for (const MaterialData& material : scene.material_data()) {
            CHECK(material.texture == no_texture);
        }
        // The geometry is there all the same.
        CHECK(elements_of<Vertex>(scene.download(SceneBufferKind::vertices)).size() == 7);
    }

    SECTION("an image wider than the device's maxImageDimension2D is a problem") {
        const std::uint32_t max_side = limits_of(*gpu).maxImageDimension2D;
        std::map<std::string, cs::assets::Texture> textures;
        textures.emplace(asset('a').sha256, cs::assets::Texture{cs::assets::build_mip_chain(
                                                cs::Image(static_cast<int>(max_side) + 1, 1))});
        textures.emplace(asset('b').sha256, graded_texture(5, 3));

        cs::world::World world;
        world.meshes = {quad_mesh()};
        world.materials.emplace_back(painting(asset('a')));
        world.materials.emplace_back(painting(asset('b')));
        world.instances = {instance(0, 0), instance(0, 1)};

        const Scene scene(*gpu, world, loader_from(textures));
        REQUIRE(scene.problems().size() == 1);
        CHECK(scene.problems()[0].asset == asset('a'));
        CHECK(scene.material_data()[0].texture == no_texture);
        REQUIRE(scene.textures().size() == 1);
        CHECK(scene.material_data()[1].texture == 0);
    }

    SECTION("a loader that throws something other than std::runtime_error is a problem too") {
        cs::world::World world;
        world.meshes = {quad_mesh()};
        world.materials.emplace_back(painting(asset('a')));
        world.instances = {instance(0, 0)};
        const TextureLoader loader = [](const AssetRef&) -> cs::assets::Texture {
            throw std::invalid_argument("deliberately malformed image");
        };
        const Scene scene(*gpu, world, loader);
        REQUIRE(scene.problems().size() == 1);
        CHECK_THAT(scene.problems()[0].reason, ContainsSubstring("deliberately malformed image"));
        CHECK(scene.material_data()[0].texture == no_texture);
    }

    SECTION("a mip chain with more levels than the image's size allows is a problem") {
        // From the gpu-reviewer: 4 x 4 needs 3 levels; two extra 1 x 1 levels would otherwise
        // reach vkCreateImage with mipLevels 5 (VUID-VkImageCreateInfo-mipLevels-00958).
        cs::assets::Texture padded = graded_texture(4, 4);
        REQUIRE(padded.levels.size() == 3);
        padded.levels.push_back(padded.levels.back());
        padded.levels.push_back(padded.levels.back());
        std::map<std::string, cs::assets::Texture> textures;
        textures.emplace(asset('a').sha256, std::move(padded));
        textures.emplace(asset('b').sha256, graded_texture(5, 3));
        cs::world::World world;
        world.meshes = {quad_mesh()};
        world.materials.emplace_back(painting(asset('a')));
        world.materials.emplace_back(painting(asset('b')));
        world.instances = {instance(0, 0), instance(0, 1)};

        const Scene scene(*gpu, world, loader_from(textures));
        REQUIRE(scene.problems().size() == 1);
        CHECK(scene.problems()[0].asset == asset('a'));
        CHECK(scene.material_data()[0].texture == no_texture);
        REQUIRE(scene.textures().size() == 1);
        CHECK(scene.material_data()[1].texture == 0);
    }
}

// ---- project_texture_loader checks the names a project file gives -------------------------------

TEST_CASE("scene: project_texture_loader loads <sha256>.<extension> from the folder",
          "[gpu][scene]") {
    const TextureLoader load = cs::gpu::project_texture_loader(skeleton_assets());
    const cs::assets::Texture texture = load(chart_asset());
    const cs::assets::Texture expected = cs::assets::load_texture(chart_path());
    REQUIRE(texture.levels.size() == expected.levels.size());
    CHECK(texture.width() == expected.width());
    CHECK(texture.height() == expected.height());
    // The same decoder on the same file: identical.
    CHECK(max_abs_difference(texture.levels[0], expected.levels[0]) == 0.0f);

    // A well-formed name that is not there: std::runtime_error, like any file that does not load.
    CHECK_THROWS_AS(load(asset('0')), std::runtime_error);
    CHECK_THROWS_AS(load(AssetRef{std::string(chart_sha256), "jpg"}), std::runtime_error);
}

TEST_CASE("scene: project_texture_loader checks names with assets::asset_file, refusing one even "
          "when a file of that name exists, and loads a .jpeg",
          "[gpu][scene]") {
    // Every hostile name is tested in libs/assets/tests/test_asset_file.cpp; here a few show that
    // the loader asks asset_file (its std::invalid_argument comes through) rather than reading
    // the file. A real PNG is planted under each, so a loader that skipped the check would load
    // it. Each name has a project folder of its own, <root>/<name>/assets, so that no plant
    // stands in for another on a case-insensitive file system.
    const TempFolder root("project_texture_loader_names");
    const auto folder_for = [&root](const std::string& name) {
        const std::filesystem::path assets = root.path() / name / "assets";
        std::filesystem::create_directories(assets);
        return assets;
    };
    const auto plant = [](const std::filesystem::path& to) {
        std::filesystem::copy_file(chart_path(), to);
    };

    SECTION("the control: the well-formed name loads") {
        const std::filesystem::path assets = folder_for("control");
        plant(assets / (std::string(chart_sha256) + ".png"));
        CHECK_NOTHROW(cs::gpu::project_texture_loader(assets)(chart_asset()));
    }
    SECTION("a .jpeg loads, as in the engine") {
        // load_texture reads the content, whatever the name says, so the PNG serves.
        const std::filesystem::path assets = folder_for("jpeg");
        plant(assets / (std::string(chart_sha256) + ".jpeg"));
        CHECK_NOTHROW(
            cs::gpu::project_texture_loader(assets)(AssetRef{std::string(chart_sha256), "jpeg"}));
    }
    SECTION("sha256 in uppercase") {
        std::string upper(chart_sha256);
        std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) {
            return (c >= 'a' && c <= 'f') ? static_cast<char>(c - 32) : c;
        });
        const std::filesystem::path assets = folder_for("upper");
        plant(assets / (upper + ".png"));
        CHECK_THROWS_AS(cs::gpu::project_texture_loader(assets)(AssetRef{upper, "png"}),
                        std::invalid_argument);
    }
    SECTION("sha256 that leaves the folder, at 64 characters") {
        const std::string kept(chart_sha256.substr(0, 61));
        const std::filesystem::path assets = folder_for("escaping");
        plant(assets.parent_path() / (kept + ".png"));
        CHECK_THROWS_AS(cs::gpu::project_texture_loader(assets)(AssetRef{"../" + kept, "png"}),
                        std::invalid_argument);
    }
    SECTION("an extension load_texture does not decode") {
        // exr is an AssetRef extension (ids.hpp) for which no image loader exists.
        const std::filesystem::path assets = folder_for("exr");
        plant(assets / (std::string(chart_sha256) + ".exr"));
        CHECK_THROWS_AS(
            cs::gpu::project_texture_loader(assets)(AssetRef{std::string(chart_sha256), "exr"}),
            std::invalid_argument);
    }
}

// ---- Invalid worlds -----------------------------------------------------------------------------

TEST_CASE("scene: a world whose instances or meshes do not hold together throws "
          "std::invalid_argument and leaves nothing allocated",
          "[gpu][scene]") {
    const std::unique_ptr<Device> gpu = make_device();
    const std::map<std::string, cs::assets::Texture> textures{
        {asset('a').sha256, graded_texture(5, 3)}};
    const TextureLoader loader = loader_from(textures);
    cs::world::World world = small_world(); // two meshes, four materials

    SECTION("an instance's mesh is out of range") {
        world.instances[1].mesh = 2;
    }
    SECTION("an instance's mesh is far out of range") {
        world.instances[0].mesh = UINT32_MAX;
    }
    SECTION("an instance's material is out of range") {
        world.instances[2].material = 4;
    }
    SECTION("a mesh's index is out of range") {
        world.meshes[0].indices[5] = 4; // the quad has 4 vertices
    }
    SECTION("a mesh's index is far out of range") {
        world.meshes[1].indices[0] = UINT32_MAX;
    }
    SECTION("a mesh has fewer normals than positions") {
        world.meshes[1].normals.pop_back();
    }
    SECTION("a mesh has more normals than positions") {
        world.meshes[0].normals.push_back(glm::dvec3(0.0, 0.0, 1.0));
    }
    SECTION("a mesh has fewer uvs than positions") {
        world.meshes[0].uvs.pop_back();
    }

    CHECK_THROWS_AS(Scene(*gpu, world, loader), std::invalid_argument);
    // "Nothing is left allocated when it throws" (scene.hpp).
    CHECK(cs::gpu::memory_in_use(*gpu).allocations == 0);
}

// ---- Edge cases: nothing to upload
// ---------------------------------------------------------------

TEST_CASE("scene: an empty world builds, with no buffers and no textures", "[gpu][scene]") {
    const std::unique_ptr<Device> gpu = make_device();
    const cs::world::World world;
    const Scene scene(*gpu, world, no_loader());

    for (const SceneBuffer* buffer :
         {&scene.vertices(), &scene.indices(), &scene.instances(), &scene.materials()}) {
        CHECK(buffer->buffer == VK_NULL_HANDLE);
        CHECK(buffer->address == 0);
        CHECK(buffer->size_bytes == 0);
    }
    CHECK(scene.meshes().empty());
    CHECK(scene.instance_data().empty());
    CHECK(scene.material_data().empty());
    CHECK(scene.textures().empty());
    CHECK(scene.problems().empty());
    CHECK(scene.memory_bytes() == 0);
    CHECK(cs::gpu::memory_in_use(*gpu).allocations == 0);

    // Assumed: an empty buffer downloads as nothing, rather than throwing.
    CHECK(scene.download(SceneBufferKind::vertices).empty());
    CHECK(scene.download(SceneBufferKind::materials).empty());
    CHECK_THROWS_AS(static_cast<void>(scene.download_texture(0, 0)), std::out_of_range);
}

TEST_CASE("scene: a world with materials but no meshes has a materials buffer only, and an empty "
          "mesh takes no room",
          "[gpu][scene]") {
    const std::unique_ptr<Device> gpu = make_device();

    SECTION("materials, no meshes or instances") {
        cs::world::World world;
        world.materials.emplace_back(cs::world::LambertMaterial{});
        world.materials.emplace_back(painting(std::nullopt));
        const Scene scene(*gpu, world, no_loader());

        CHECK(scene.vertices().buffer == VK_NULL_HANDLE);
        CHECK(scene.vertices().size_bytes == 0);
        CHECK(scene.indices().buffer == VK_NULL_HANDLE);
        CHECK(scene.instances().buffer == VK_NULL_HANDLE);
        CHECK(scene.materials().buffer != VK_NULL_HANDLE);
        CHECK(scene.materials().size_bytes == 2 * sizeof(MaterialData));
        CHECK(same_bytes(scene.download(SceneBufferKind::materials), scene.material_data()));
    }

    SECTION("an empty mesh before a quad") {
        cs::world::World world;
        world.meshes = {cs::geom::Mesh{}, quad_mesh()};
        world.materials.emplace_back(cs::world::LambertMaterial{});
        world.instances = {instance(1, 0)};
        const Scene scene(*gpu, world, no_loader());

        const std::span<const MeshRange> meshes = scene.meshes();
        REQUIRE(meshes.size() == 2);
        CHECK(meshes[0].vertex_count == 0);
        CHECK(meshes[0].index_count == 0);
        CHECK(meshes[1].first_vertex == 0);
        CHECK(meshes[1].vertex_count == 4);
        CHECK(meshes[1].first_index == 0);
        CHECK(meshes[1].index_count == 6);
        REQUIRE(scene.instance_data().size() == 1);
        CHECK(scene.instance_data()[0].mesh == 1);
        CHECK(scene.instance_data()[0].first_index == 0);
        CHECK(scene.instance_data()[0].vertex_offset == 0);

        const std::vector<Vertex> downloaded =
            elements_of<Vertex>(scene.download(SceneBufferKind::vertices));
        const std::vector<Vertex> expected = expected_vertices(world);
        REQUIRE(downloaded.size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            INFO("vertex " << i);
            CHECK(same_vertex(downloaded[i], expected[i]));
        }
    }
}

// ---- Criterion 3: memory from VMA, and zero allocations after the scene
// ---------------------------

TEST_CASE("scene: memory in use is reported from VMA, and is zero allocations again after the "
          "scene is destroyed",
          "[gpu][scene]") {
    const std::unique_ptr<Device> gpu = make_device();
    const cs::world::World world = small_world();
    const std::map<std::string, cs::assets::Texture> textures{
        {asset('a').sha256, graded_texture(5, 3)}};

    // Nothing else allocates through VMA in this test.
    const MemoryInUse before = cs::gpu::memory_in_use(*gpu);
    CHECK(before.allocations == 0);
    CHECK(before.allocation_bytes == 0);

    std::optional<Scene> scene;
    scene.emplace(*gpu, world, loader_from(textures));
    REQUIRE(scene->problems().empty());

    const MemoryInUse during = cs::gpu::memory_in_use(*gpu);
    CHECK(during.allocations > before.allocations);
    CHECK(during.allocation_bytes > before.allocation_bytes);
    CHECK(during.block_bytes >= during.allocation_bytes);

    // The scene's own count: at least the bytes it holds. The texture is 5x3 + 2x1 + 1x1 = 18
    // texels of RGBA16F, 8 bytes each; alignment and the device's image layout only add to it.
    const VkDeviceSize held = scene->vertices().size_bytes + scene->indices().size_bytes +
                              scene->instances().size_bytes + scene->materials().size_bytes +
                              18 * 8;
    CHECK(scene->memory_bytes() >= held);
    // Both are sums of the sizes of the same VMA allocations, the scene's being all there are.
    CHECK(during.allocation_bytes - before.allocation_bytes == scene->memory_bytes());

    SECTION("downloading leaves nothing allocated behind") {
        static_cast<void>(scene->download(SceneBufferKind::vertices));
        static_cast<void>(scene->download(SceneBufferKind::materials));
        static_cast<void>(scene->download_texture(0, 0));
        static_cast<void>(scene->download_texture(0, 2));
        const MemoryInUse after_downloads = cs::gpu::memory_in_use(*gpu);
        CHECK(after_downloads.allocations == during.allocations);
        CHECK(after_downloads.allocation_bytes == during.allocation_bytes);
    }

    scene.reset();
    const MemoryInUse after = cs::gpu::memory_in_use(*gpu);
    CHECK(after.allocations == 0);
    CHECK(after.allocation_bytes == 0);
}

TEST_CASE("scene: two scenes on one device count together, and each frees its own",
          "[gpu][scene]") {
    const std::unique_ptr<Device> gpu = make_device();
    const cs::world::World world = small_world();
    const std::map<std::string, cs::assets::Texture> textures{
        {asset('a').sha256, graded_texture(5, 3)}};

    std::optional<Scene> first;
    std::optional<Scene> second;
    first.emplace(*gpu, world, loader_from(textures));
    const MemoryInUse one = cs::gpu::memory_in_use(*gpu);
    second.emplace(*gpu, world, loader_from(textures));
    const MemoryInUse two = cs::gpu::memory_in_use(*gpu);

    CHECK(first->memory_bytes() == second->memory_bytes()); // the same world
    CHECK(two.allocations == 2 * one.allocations);
    CHECK(two.allocation_bytes == one.allocation_bytes + second->memory_bytes());

    first.reset();
    const MemoryInUse left = cs::gpu::memory_in_use(*gpu);
    CHECK(left.allocations == one.allocations);
    CHECK(left.allocation_bytes == second->memory_bytes());
    // The survivor is intact.
    CHECK(elements_of<Vertex>(second->download(SceneBufferKind::vertices)).size() == 7);

    second.reset();
    CHECK(cs::gpu::memory_in_use(*gpu).allocations == 0);
}
