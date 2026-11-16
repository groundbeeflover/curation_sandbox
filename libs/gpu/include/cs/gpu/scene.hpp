// SPDX-License-Identifier: Apache-2.0
#pragma once

// The render world on the GPU
// ---------------------------
// A Scene is a world::World uploaded for the GPU tiers: every mesh's vertices and indices, one
// record per instance, one per material, and the paintings' images as textures with their full
// mip chains. It is built once, through one staging buffer and one queue submission, and then
// only read; it changes only by being built again (incremental updates are M2-24).
//
// Layout. Every mesh's vertices go into one vertex buffer and its indices, still counted from
// the mesh's own first vertex, into one index buffer; MeshRange says where each mesh lies, so
// an instance draws with vkCmdDrawIndexed(index_count, 1, first_index, vertex_offset, i). The
// pipelines have no vertex input (pipeline.hpp): shaders read the buffers through their device
// addresses, with scalar layout (C++ packing, as the structs below are written), and SV_VertexID
// is the index into the mesh, the device enabling shaderDrawParameters (device.hpp).
//
// Units and colours are the world's: metres, linear Rec. 709. Values that were doubles in the
// world are floats here, rounded to nearest.
//
// Textures are VK_FORMAT_R16G16B16A16_SFLOAT with alpha 1: Vulkan does not require RGB32F to be
// sampleable, and half floats hold linear albedo in [0, 1] to within 4.9e-4, at half the memory
// of RGBA32F. They are converted while the staging buffer is filled. After the upload, every
// texture is in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL and every write is visible to the
// shader, index and copy reads of later commands on the device's queue.

#include <cs/assets/texture.hpp>
#include <cs/core/image.hpp>
#include <cs/gpu/device.hpp>
#include <cs/world/ids.hpp>
#include <cs/world/world.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace cs::gpu {

// ---- What the buffers hold ---------------------------------------------------------------------

// One vertex of a mesh, in the mesh's own coordinates: 32 bytes.
struct Vertex {
    glm::vec3 position_m{0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f}; // unit
    glm::vec2 uv{0.0f};                 // geom's conventions: metres along the surface, mostly
};
static_assert(sizeof(Vertex) == 32);

// Where one mesh of the world lies in the vertex and index buffers, in elements.
struct MeshRange {
    std::uint32_t first_vertex = 0; // the vertexOffset of its draws
    std::uint32_t vertex_count = 0;
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0; // three per triangle
};
static_assert(sizeof(MeshRange) == 16);

// One instance of the world, in the order of World::instances: 80 bytes.
struct InstanceData {
    // The mesh's coordinates to the world, column-major. The world's transforms are rotations and
    // translations, so its upper 3x3 turns normals too.
    glm::mat4 transform{1.0f};
    std::uint32_t mesh = 0;          // into the world's meshes and Scene::meshes()
    std::uint32_t material = 0;      // into the materials buffer, in the order of World::materials
    std::uint32_t first_index = 0;   // the mesh's MeshRange, copied, so a shader or a draw loop
    std::uint32_t vertex_offset = 0; // needs nothing else
};
static_assert(sizeof(InstanceData) == 80);

// What MaterialData::kind says.
enum class MaterialKind : std::uint32_t {
    lambert = 0,  // world::LambertMaterial: base_colour is its albedo
    painting = 1, // world::PaintingSurface: the texture's texels, or base_colour without one
};

// No texture: MaterialData::texture for a material without one, or whose image did not load.
inline constexpr std::uint32_t no_texture = 0xFFFF'FFFFu;

// One material of the world, in the order of World::materials: 48 bytes. A painting's values
// are its PaintingMaterial's base and coat; the rest of it (gilding, fuzz, glazing) is not read
// by any tier yet.
struct MaterialData {
    glm::vec3 base_colour{0.8f}; // linear albedo
    MaterialKind kind = MaterialKind::lambert;
    std::uint32_t texture = no_texture; // into Scene::textures()
    float roughness = 1.0f;
    float specular_weight = 0.0f;
    float coat_weight = 0.0f;
    float coat_roughness = 0.0f;
    float coat_ior = 1.5f;
    std::uint32_t padding[2]{};
};
static_assert(sizeof(MaterialData) == 48);

// ---- Textures ----------------------------------------------------------------------------------

// Gives the decoded image of an asset the world names, with its full mip chain
// (assets::load_texture builds one). It throws std::runtime_error when it cannot: the scene
// then shows that painting's base colour and says why in Scene::problems().
using TextureLoader = std::function<assets::Texture(const world::AssetRef&)>;

// Loads assets from a project's assets/ folder, named <sha256>.<extension> (ADR 0007), with
// assets::load_texture. The names come from a project file, so they are checked before a path is
// made of them: the hash must be 64 lowercase hexadecimal digits and the extension png, jpg or
// jpeg; anything else throws std::runtime_error, like a file that does not load.
TextureLoader project_texture_loader(std::filesystem::path assets_folder);

// One painting's image on the GPU.
struct Texture {
    world::AssetRef asset; // the image it holds; one texture per asset, however many use it
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mip_levels = 0; // the full chain, down to 1 x 1
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE; // 2D, every mip level
};

// An asset the scene could not upload, and why. Its materials show their base colour.
struct TextureProblem {
    world::AssetRef asset;
    std::string reason;
};

// ---- The scene ---------------------------------------------------------------------------------

// One device-local buffer, usable as a storage buffer, through its device address, and as the
// source and destination of copies; the index buffer also as an index buffer. A buffer with
// nothing to hold (a world with no meshes) is VK_NULL_HANDLE, with address 0 and size 0.
struct SceneBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    VkDeviceSize size_bytes = 0;
};

// Which buffer download() reads.
enum class SceneBufferKind { vertices, indices, instances, materials };

class Scene {
  public:
    // Uploads the world and waits for the upload to finish, then frees the staging memory.
    // Throws std::invalid_argument if an instance names a mesh or material that is not in the
    // world, or a mesh has an index out of range or not one normal and uv per position;
    // std::length_error if the meshes have more vertices or indices together than 32-bit draws
    // can count; std::runtime_error on a Vulkan failure, when the upload is larger than the device
    // can allocate as one buffer, or when it has not finished within a minute (the exception
    // leaves once the GPU has finished with it, or the device is lost). Nothing is left
    // allocated when it throws. An image the loader cannot give, or the device cannot hold, is
    // not an error: see problems().
    Scene(const Device& device, const world::World& world, const TextureLoader& load_texture);
    // Frees everything at once: every submission that reads the scene must have finished.
    // Destroy it before its Device.
    ~Scene();

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&&) = delete;
    Scene& operator=(Scene&&) = delete;

    [[nodiscard]] const SceneBuffer& vertices() const { return buffer(SceneBufferKind::vertices); }
    [[nodiscard]] const SceneBuffer& indices() const { return buffer(SceneBufferKind::indices); }
    [[nodiscard]] const SceneBuffer& instances() const {
        return buffer(SceneBufferKind::instances);
    }
    [[nodiscard]] const SceneBuffer& materials() const {
        return buffer(SceneBufferKind::materials);
    }
    // vertices: Vertex; indices: std::uint32_t; instances: InstanceData; materials: MaterialData.
    [[nodiscard]] const SceneBuffer& buffer(SceneBufferKind which) const {
        return buffers_[static_cast<std::size_t>(which)];
    }

    // Host copies of what the GPU holds, for draw loops.
    [[nodiscard]] std::span<const MeshRange> meshes() const { return meshes_; }
    [[nodiscard]] std::span<const InstanceData> instance_data() const { return instance_data_; }
    [[nodiscard]] std::span<const MaterialData> material_data() const { return material_data_; }

    [[nodiscard]] std::span<const Texture> textures() const { return textures_; }
    [[nodiscard]] std::span<const TextureProblem> problems() const { return problems_; }

    // The device memory the scene's buffers and images take, from VMA: the sizes of their
    // allocations, alignment included.
    [[nodiscard]] VkDeviceSize memory_bytes() const;

    // For tests and diagnostics: copies a buffer, or one mip level of a texture, back to the host
    // and waits for it. A texture level comes back as linear RGB (alpha dropped), row 0 at the
    // top. Throws std::out_of_range for a texture or level that does not exist, and
    // std::runtime_error on a Vulkan failure. It submits to the device's queue, so not while
    // another thread uses that queue.
    [[nodiscard]] std::vector<std::byte> download(SceneBufferKind which) const;
    [[nodiscard]] Image download_texture(std::uint32_t texture, std::uint32_t level) const;

  private:
    static constexpr std::size_t buffer_count = 4; // the SceneBufferKinds

    const Device* device_ = nullptr;
    std::array<SceneBuffer, buffer_count> buffers_{};
    std::array<VmaAllocation, buffer_count> buffer_allocations_{};
    std::vector<MeshRange> meshes_;
    std::vector<InstanceData> instance_data_;
    std::vector<MaterialData> material_data_;
    std::vector<Texture> textures_;
    std::vector<VmaAllocation> texture_allocations_; // one per texture
    std::vector<TextureProblem> problems_;

    void destroy() noexcept;
};

// ---- Memory ------------------------------------------------------------------------------------

// Everything allocated through a device's VMA allocator, from vmaCalculateStatistics: every
// scene, target and buffer alive on it.
struct MemoryInUse {
    std::uint64_t allocations = 0;      // VMA allocations alive
    std::uint64_t allocation_bytes = 0; // their sizes
    std::uint64_t block_bytes = 0;      // the device memory VMA holds for them, free space included
};

[[nodiscard]] MemoryInUse memory_in_use(const Device& device);

} // namespace cs::gpu
