// SPDX-License-Identifier: Apache-2.0

// The skeleton project on the GPU (issue M1-21, criteria 2 and 3): the document loaded from
// tests/scenes/skeleton, compiled to a world and uploaded as a gpu::Scene with the project's own
// texture loader. It belongs here because gpu may not depend on the document (blueprint
// section 7), so its own tests cannot load a project.
//
// This executable has Catch2's own main, not the gpu tests' harness (libs/gpu/tests/main.cpp), so
// it brings the two things of that harness it needs: a device that skips without a GPU and fails
// without one under CS_REQUIRE_GPU=1 (ADR 0010), and a validation handler under which any
// validation warning or error fails the test.

#include <cs/assets/texture.hpp>
#include <cs/core/image.hpp>
#include <cs/doc/document.hpp>
#include <cs/doc/project_io.hpp>
#include <cs/geom/mesh.hpp>
#include <cs/gpu/device.hpp>
#include <cs/gpu/scene.hpp>
#include <cs/world/compile.hpp>
#include <cs/world/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

// ---- A device for this test, as libs/gpu/tests/gpu_test.hpp makes one ---------------------------

#ifdef NDEBUG
constexpr bool debug_build = false;
#else
constexpr bool debug_build = true;
#endif

// CS_REQUIRE_GPU=1 (CI, /ship): no device is a failure rather than a skip. MSVC deprecates
// getenv, so Windows reads through _dupenv_s.
bool gpu_required() {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, "CS_REQUIRE_GPU") != 0 || value == nullptr) {
        return false;
    }
    const std::string text(value);
    std::free(value);
#else
    const char* value = std::getenv("CS_REQUIRE_GPU");
    if (value == nullptr) {
        return false;
    }
    const std::string text(value);
#endif
    return !text.empty() && text != "0";
}

// In a debug build under CS_REQUIRE_GPU=1 the validation layer must load (ADR 0011).
bool validation_layer_required() {
    return debug_build && gpu_required();
}

std::unique_ptr<cs::gpu::Device> make_device() {
    cs::gpu::DeviceOptions options;
    options.validation = validation_layer_required() ? cs::gpu::Validation::required
                                                     : cs::gpu::Validation::if_available;
    // Returned from after the handler, as gpu_test.hpp's make_device does: a return after FAIL
    // and SKIP alone is unreachable (MSVC's warning C4702 in debug builds).
    std::unique_ptr<cs::gpu::Device> device;
    try {
        device = std::make_unique<cs::gpu::Device>(options);
    } catch (const cs::gpu::NoDeviceError& e) {
        if (gpu_required()) {
            FAIL("no Vulkan device was found, and CS_REQUIRE_GPU=1 requires one: " << e.what());
        } else {
            SKIP("no Vulkan device was found: " << e.what());
        }
    }
    return device;
}

// ---- Validation messages fail the test ---------------------------------------------------------

// Collects every validation warning and error while it lives, from whatever thread reports it,
// and puts the previous handler back when it goes. Messages of kind general (the loader listing
// its drivers and layers) are printed and fail nothing, as in the gpu tests' harness. The test
// checks messages() once the device is gone, so that what the device reports when it is
// destroyed (objects left alive) is counted too.
class ValidationCollector {
  public:
    ValidationCollector()
        : previous_(cs::gpu::set_validation_handler(
              [this](const cs::gpu::ValidationMessage& m) { receive(m); })) {}
    ~ValidationCollector() { cs::gpu::set_validation_handler(std::move(previous_)); }
    ValidationCollector(const ValidationCollector&) = delete;
    ValidationCollector& operator=(const ValidationCollector&) = delete;

    std::vector<std::string> messages() const {
        const std::lock_guard lock(mutex_);
        return messages_;
    }

  private:
    void receive(const cs::gpu::ValidationMessage& message) {
        using Message = cs::gpu::ValidationMessage;
        const std::string text = cs::gpu::to_string(message);
        if (message.severity < Message::Severity::warning ||
            message.kind == Message::Kind::general) {
            std::cerr << text << '\n';
            return;
        }
        const std::lock_guard lock(mutex_);
        messages_.push_back(text);
    }

    // In this order: the handler, which may be called at once, is installed last.
    mutable std::mutex mutex_;
    std::vector<std::string> messages_;
    cs::gpu::ValidationHandler previous_;
};

// ---- Comparisons -------------------------------------------------------------------------------

// Textures are uploaded as half floats (scene.hpp): 11 significant bits, so the spacing of half
// floats in [0.5, 1) is 2^-11 = 4.9e-4, and smaller below. A linear albedo in [0, 1] comes back
// within that even from a conversion that truncates, half of it from one that rounds. 1e-3 is
// the issue's criterion 2.
constexpr float texel_tolerance = 1e-3f;

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

template <typename T> std::vector<T> elements_of(const std::vector<std::byte>& bytes) {
    REQUIRE(bytes.size() % sizeof(T) == 0);
    std::vector<T> elements(bytes.size() / sizeof(T));
    if (!bytes.empty()) {
        std::memcpy(elements.data(), bytes.data(), bytes.size());
    }
    return elements;
}

} // namespace

TEST_CASE("the skeleton project uploads to the GPU: its vertices exactly, its chart within 1e-3, "
          "and no VMA allocation left after",
          "[integration][gpu]") {
    const std::filesystem::path project =
        std::filesystem::path(CS_SOURCE_DIR) / "tests" / "scenes" / "skeleton";
    const cs::doc::Document document = cs::doc::load_project_folder(project);
    const cs::world::World world = cs::world::compile(document);
    REQUIRE(world.problems.empty());
    REQUIRE(world.exhibits.size() == 1);

    // The chart's painting, and its image (tests/scenes/skeleton/README.md).
    const cs::world::Instance& painting_instance =
        world.instances.at(world.exhibits.front().painting_instance);
    REQUIRE(painting_instance.part == cs::world::Part::painting);
    const std::uint32_t painting_material = painting_instance.material;
    const auto* surface =
        std::get_if<cs::world::PaintingSurface>(&world.materials.at(painting_material));
    REQUIRE(surface != nullptr);
    REQUIRE(surface->image.has_value());
    const std::filesystem::path chart_file =
        project / "assets" / (surface->image->sha256 + "." + surface->image->extension);
    const cs::assets::Texture expected_chart = cs::assets::load_texture(chart_file);

    // The world's vertices as the scene must hold them: mesh after mesh, each double rounded to
    // the nearest float (static_cast does so). And its indices, counted from each mesh's own
    // first vertex.
    std::vector<cs::gpu::Vertex> expected_vertices;
    std::vector<std::uint32_t> expected_indices;
    for (const cs::geom::Mesh& mesh : world.meshes) {
        for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
            cs::gpu::Vertex v;
            v.position_m = glm::vec3(mesh.positions[i]);
            v.normal = glm::vec3(mesh.normals[i]);
            v.uv = glm::vec2(mesh.uvs[i]);
            expected_vertices.push_back(v);
        }
        expected_indices.insert(expected_indices.end(), mesh.indices.begin(), mesh.indices.end());
    }
    REQUIRE_FALSE(expected_vertices.empty());

    // Declared first, so it outlives the device and sees what the device reports as it goes.
    const ValidationCollector validation;
    {
        const std::unique_ptr<cs::gpu::Device> gpu = make_device();
        {
            const cs::gpu::Scene scene(*gpu, world,
                                       cs::gpu::project_texture_loader(project / "assets"));
            for (const cs::gpu::TextureProblem& problem : scene.problems()) {
                UNSCOPED_INFO("texture problem: " << problem.reason);
            }
            CHECK(scene.problems().empty());

            // Criterion 2: the vertex buffer read back equals the world's vertices exactly.
            const std::vector<cs::gpu::Vertex> vertices =
                elements_of<cs::gpu::Vertex>(scene.download(cs::gpu::SceneBufferKind::vertices));
            REQUIRE(vertices.size() == expected_vertices.size());
            std::size_t wrong = 0;
            for (std::size_t i = 0; i < vertices.size(); ++i) {
                const cs::gpu::Vertex& a = vertices[i];
                const cs::gpu::Vertex& b = expected_vertices[i];
                // Exact on purpose: both are the same double rounded to float once.
                if (!(a.position_m == b.position_m && a.normal == b.normal && a.uv == b.uv)) {
                    if (wrong == 0) {
                        UNSCOPED_INFO("first wrong vertex: " << i);
                    }
                    ++wrong;
                }
            }
            CHECK(wrong == 0);
            CHECK(elements_of<std::uint32_t>(scene.download(cs::gpu::SceneBufferKind::indices)) ==
                  expected_indices);

            // Criterion 2: mip 0 of the painting's texture equals the linear image within 1e-3.
            REQUIRE(scene.material_data().size() == world.materials.size());
            const std::uint32_t texture = scene.material_data()[painting_material].texture;
            REQUIRE(texture != cs::gpu::no_texture);
            REQUIRE(texture < scene.textures().size());
            CHECK(scene.textures()[texture].asset == *surface->image);
            CHECK(scene.textures()[texture].mip_levels == expected_chart.levels.size());
            const cs::Image mip0 = scene.download_texture(texture, 0);
            CHECK(max_abs_difference(mip0, expected_chart.levels[0]) <= texel_tolerance);

            // Criterion 3: memory in use comes from VMA.
            CHECK(scene.memory_bytes() > 0);
            CHECK(cs::gpu::memory_in_use(*gpu).allocations > 0);
        }
        // Criterion 3: VMA reports zero allocations once the scene is destroyed.
        const cs::gpu::MemoryInUse after = cs::gpu::memory_in_use(*gpu);
        CHECK(after.allocations == 0);
        CHECK(after.allocation_bytes == 0);
    }

    // Criterion 3: validation clean, the device's own destruction included.
    const std::vector<std::string> messages = validation.messages();
    for (const std::string& message : messages) {
        UNSCOPED_INFO(message);
    }
    CHECK(messages.empty());
}
