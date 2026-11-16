// SPDX-License-Identifier: Apache-2.0

#include "cs/gpu/scene.hpp"

#include "upload.hpp"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace cs::gpu {

namespace {

constexpr const char* who = "gpu::Scene";

constexpr VkFormat texture_format = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr std::size_t texture_channels = 4;
constexpr VkDeviceSize texel_bytes = texture_channels * sizeof(std::uint16_t);
constexpr VkImageUsageFlags texture_usage =
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
const std::uint16_t half_one = glm::packHalf1x16(1.0f);

constexpr VkBufferUsageFlags buffer_usage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
    VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

// Every section of the staging buffer starts at a multiple of 16 bytes: copies to an image need
// a multiple of the texel size (8), and 16 serves every struct here.
constexpr VkDeviceSize staging_alignment = 16;

VkDeviceSize align_up(VkDeviceSize bytes) {
    return (bytes + staging_alignment - 1) / staging_alignment * staging_alignment;
}

std::uint32_t level_side(std::uint32_t side, std::uint32_t level) {
    return std::max(side >> level, 1u);
}

[[noreturn]] void invalid(const std::string& what) {
    throw std::invalid_argument(std::string(who) + ": " + what);
}

// ---- The world, checked and flattened ----------------------------------------------------------

void validate(const world::World& world) {
    for (std::size_t m = 0; m < world.meshes.size(); ++m) {
        const geom::Mesh& mesh = world.meshes[m];
        if (mesh.normals.size() != mesh.positions.size() ||
            mesh.uvs.size() != mesh.positions.size())
            invalid("mesh " + std::to_string(m) + " has not one normal and one uv per position");
        if (mesh.indices.size() % 3 != 0)
            invalid("mesh " + std::to_string(m) +
                    " has a number of indices that is not a multiple of 3");
        for (const std::uint32_t index : mesh.indices) {
            if (index >= mesh.positions.size())
                invalid("mesh " + std::to_string(m) + " has index " + std::to_string(index) +
                        ", out of range");
        }
    }
    for (std::size_t i = 0; i < world.instances.size(); ++i) {
        const world::Instance& instance = world.instances[i];
        if (instance.mesh >= world.meshes.size())
            invalid("instance " + std::to_string(i) + " names mesh " +
                    std::to_string(instance.mesh) + ", which is not in the world");
        if (instance.material >= world.materials.size())
            invalid("instance " + std::to_string(i) + " names material " +
                    std::to_string(instance.material) + ", which is not in the world");
    }
}

// Every mesh's vertices and indices, one after another.
struct Geometry {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<MeshRange> ranges;
};

Geometry flatten(const std::vector<geom::Mesh>& meshes) {
    // vertexOffset is an int32_t, first_index a uint32_t.
    std::uint64_t vertex_total = 0;
    std::uint64_t index_total = 0;
    for (const geom::Mesh& mesh : meshes) {
        vertex_total += mesh.positions.size();
        index_total += mesh.indices.size();
    }
    if (vertex_total > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) ||
        index_total > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error(std::string(who) + ": the world's meshes have " +
                                std::to_string(vertex_total) + " vertices and " +
                                std::to_string(index_total) +
                                " indices, more than 32-bit draws can count");

    Geometry geometry;
    geometry.vertices.reserve(static_cast<std::size_t>(vertex_total));
    geometry.indices.reserve(static_cast<std::size_t>(index_total));
    geometry.ranges.reserve(meshes.size());
    for (const geom::Mesh& mesh : meshes) {
        geometry.ranges.push_back({static_cast<std::uint32_t>(geometry.vertices.size()),
                                   static_cast<std::uint32_t>(mesh.positions.size()),
                                   static_cast<std::uint32_t>(geometry.indices.size()),
                                   static_cast<std::uint32_t>(mesh.indices.size())});
        for (std::size_t v = 0; v < mesh.positions.size(); ++v)
            geometry.vertices.push_back(
                {glm::vec3(mesh.positions[v]), glm::vec3(mesh.normals[v]), glm::vec2(mesh.uvs[v])});
        geometry.indices.insert(geometry.indices.end(), mesh.indices.begin(), mesh.indices.end());
    }
    return geometry;
}

// A material's values, without its texture, which the scene assigns.
MaterialData material_values(const world::Material& material) {
    MaterialData data;
    if (const auto* lambert = std::get_if<world::LambertMaterial>(&material)) {
        data.kind = MaterialKind::lambert;
        data.base_colour = glm::vec3(lambert->albedo);
        return data;
    }
    const material::PaintingMaterial& p = std::get<world::PaintingSurface>(material).parameters;
    data.kind = MaterialKind::painting;
    data.base_colour = glm::vec3(p.base.color);
    data.roughness = static_cast<float>(p.base.roughness);
    data.specular_weight = static_cast<float>(p.base.specular_weight);
    data.coat_weight = static_cast<float>(p.coat.weight);
    data.coat_roughness = static_cast<float>(p.coat.roughness);
    data.coat_ior = static_cast<float>(p.coat.ior);
    return data;
}

// ---- Textures, checked -------------------------------------------------------------------------

// Why the device cannot hold this texture as given, or empty when it can. The loader is anyone's
// function, so its chain is checked as load_texture promises it: exactly mip_level_count levels,
// each half the one above, rounded down and at least 1. Extra 1 x 1 levels would otherwise pass,
// and the image would be made with more levels than its size allows.
std::string texture_problem(const assets::Texture& texture, VkPhysicalDevice physical_device) {
    if (texture.levels.empty() || texture.width() <= 0 || texture.height() <= 0)
        return "the image is empty";
    if (texture.levels.size() !=
        static_cast<std::size_t>(assets::mip_level_count(texture.width(), texture.height())))
        return "the mip chain does not have one level for each halving down to 1 x 1";
    const auto width = static_cast<std::uint32_t>(texture.width());
    const auto height = static_cast<std::uint32_t>(texture.height());
    const auto levels = static_cast<std::uint32_t>(texture.levels.size());
    VkDeviceSize bytes = 0;
    for (std::uint32_t level = 0; level < levels; ++level) {
        const Image& image = texture.levels[level];
        if (static_cast<std::uint32_t>(image.width()) != level_side(width, level) ||
            static_cast<std::uint32_t>(image.height()) != level_side(height, level))
            return "mip level " + std::to_string(level) + " is not half the level above";
        bytes += VkDeviceSize{level_side(width, level)} * level_side(height, level) * texel_bytes;
    }
    VkImageFormatProperties properties{};
    if (vkGetPhysicalDeviceImageFormatProperties(physical_device, texture_format, VK_IMAGE_TYPE_2D,
                                                 VK_IMAGE_TILING_OPTIMAL, texture_usage, 0,
                                                 &properties) != VK_SUCCESS)
        return "the device cannot sample RGBA16F textures";
    if (width > properties.maxExtent.width || height > properties.maxExtent.height ||
        levels > properties.maxMipLevels || bytes > properties.maxResourceSize)
        return std::to_string(width) + " x " + std::to_string(height) +
               " pixels is larger than the device can hold as one texture";
    return {};
}

bool is_sha256(const std::string& text) {
    return text.size() == 64 && std::ranges::all_of(text, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

// ---- Staging -----------------------------------------------------------------------------------

// One buffer's bytes in the staging buffer. Only buffers with something to hold have one.
struct Section {
    SceneBufferKind buffer;
    const std::byte* data;
    VkDeviceSize size_bytes;
    VkDeviceSize offset = 0;
};

template <typename T> Section section(SceneBufferKind buffer, const std::vector<T>& values) {
    static_assert(std::is_trivially_copyable_v<T>);
    return {buffer, reinterpret_cast<const std::byte*>(values.data()),
            VkDeviceSize{values.size()} * sizeof(T)};
}

// A texture waiting to be staged, in the order of Scene::textures(): its decoded levels and
// where each one goes in the staging buffer.
struct PendingTexture {
    assets::Texture pixels;
    std::vector<VkDeviceSize> level_offsets;
};

// One level as half floats, alpha 1, rows packed from the top.
void write_half_floats(const Image& image, std::byte* out) {
    std::uint16_t texel[texture_channels];
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const Rgb& rgb = image.at(x, y);
            texel[0] = glm::packHalf1x16(rgb.r);
            texel[1] = glm::packHalf1x16(rgb.g);
            texel[2] = glm::packHalf1x16(rgb.b);
            texel[3] = half_one;
            std::memcpy(out, texel, sizeof texel);
            out += sizeof texel;
        }
    }
}

// Every copy of the upload, between the barriers that order it.
void record_upload(VkCommandBuffer cmd, VkBuffer staging, std::span<const Section> sections,
                   std::span<const SceneBuffer> buffers, std::span<const Texture> textures,
                   std::span<const PendingTexture> pending) {
    using detail::Access;
    const auto every_level = [&](Access from, Access to, VkImageLayout old_layout,
                                 VkImageLayout new_layout) {
        std::vector<VkImageMemoryBarrier2> barriers;
        for (const Texture& texture : textures)
            barriers.push_back(detail::image_barrier(texture.image, 0, texture.mip_levels, from, to,
                                                     old_layout, new_layout));
        return barriers;
    };

    // New images: nothing to wait for.
    if (!textures.empty())
        detail::record_barriers(
            cmd, {},
            every_level({}, {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT},
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));

    for (const Section& s : sections) {
        const VkBufferCopy copy{s.offset, 0, s.size_bytes};
        vkCmdCopyBuffer(cmd, staging, buffers[static_cast<std::size_t>(s.buffer)].buffer, 1, &copy);
    }
    for (std::size_t t = 0; t < textures.size(); ++t) {
        std::vector<VkBufferImageCopy> regions;
        for (std::uint32_t level = 0; level < textures[t].mip_levels; ++level) {
            VkBufferImageCopy& region = regions.emplace_back();
            region.bufferOffset = pending[t].level_offsets[level]; // tightly packed rows
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            region.imageExtent = {level_side(textures[t].width, level),
                                  level_side(textures[t].height, level), 1};
        }
        vkCmdCopyBufferToImage(cmd, staging, textures[t].image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<std::uint32_t>(regions.size()), regions.data());
    }

    // The copies' writes, visible to every later read on the queue, in this submission's order or
    // a later one's: the tiers' shaders and index fetches, and download()'s copies.
    const Access copied{VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
    const Access any_read{VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT};
    VkMemoryBarrier2 written{};
    written.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    written.srcStageMask = copied.stages;
    written.srcAccessMask = copied.accesses;
    written.dstStageMask = any_read.stages;
    written.dstAccessMask = any_read.accesses;
    detail::record_barriers(cmd, {&written, 1},
                            every_level(copied, any_read, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
}

} // namespace

// ---- Loading from a project folder -------------------------------------------------------------

TextureLoader project_texture_loader(std::filesystem::path assets_folder) {
    return [folder = std::move(assets_folder)](const world::AssetRef& asset) {
        // Both names come from a project file: nothing in them may climb out of the folder.
        if (!is_sha256(asset.sha256))
            throw std::runtime_error("the asset name is not a SHA-256 of 64 lowercase "
                                     "hexadecimal digits");
        if (asset.extension != "png" && asset.extension != "jpg" && asset.extension != "jpeg")
            throw std::runtime_error("only PNG and JPEG images can be loaded (the asset's "
                                     "extension is not png, jpg or jpeg)");
        return assets::load_texture(folder / (asset.sha256 + "." + asset.extension));
    };
}

// ---- The scene ---------------------------------------------------------------------------------

Scene::Scene(const Device& device, const world::World& world, const TextureLoader& load_texture)
    : device_(&device) {
    validate(world);
    const Geometry geometry = flatten(world.meshes);
    meshes_ = geometry.ranges;

    instance_data_.reserve(world.instances.size());
    for (const world::Instance& instance : world.instances) {
        InstanceData data;
        data.transform = glm::mat4(instance.transform);
        data.mesh = instance.mesh;
        data.material = instance.material;
        data.first_index = meshes_[instance.mesh].first_index;
        data.vertex_offset = meshes_[instance.mesh].first_vertex;
        instance_data_.push_back(data);
    }

    // Materials, and the textures they name: each asset loaded once, or reported once.
    std::vector<PendingTexture> pending;             // parallel to textures_
    std::map<std::string, std::uint32_t> texture_of; // "<sha256>.<extension>", or no_texture
    const auto texture_for = [&](const world::AssetRef& asset) {
        const auto [entry, added] =
            texture_of.try_emplace(asset.sha256 + "." + asset.extension, no_texture);
        if (!added)
            return entry->second;
        std::string reason;
        try {
            assets::Texture texture = load_texture(asset);
            reason = texture_problem(texture, device.physical_device());
            if (reason.empty()) {
                entry->second = static_cast<std::uint32_t>(textures_.size());
                textures_.push_back({asset, static_cast<std::uint32_t>(texture.width()),
                                     static_cast<std::uint32_t>(texture.height()),
                                     static_cast<std::uint32_t>(texture.levels.size())});
                pending.push_back({std::move(texture), {}});
                return entry->second;
            }
        } catch (const std::exception& e) {
            // Whatever the loader throws, out of memory included, costs one painting its image,
            // not the scene.
            reason = e.what();
        }
        problems_.push_back({asset, reason});
        return no_texture;
    };
    material_data_.reserve(world.materials.size());
    for (const world::Material& material : world.materials) {
        MaterialData& data = material_data_.emplace_back(material_values(material));
        if (const auto* painting = std::get_if<world::PaintingSurface>(&material);
            painting != nullptr && painting->image)
            data.texture = texture_for(*painting->image);
    }

    // The staging buffer's layout: the buffers with something to hold, then every texture level.
    std::vector<Section> sections;
    VkDeviceSize staging_bytes = 0;
    for (Section s : {section(SceneBufferKind::vertices, geometry.vertices),
                      section(SceneBufferKind::indices, geometry.indices),
                      section(SceneBufferKind::instances, instance_data_),
                      section(SceneBufferKind::materials, material_data_)}) {
        if (s.size_bytes == 0)
            continue;
        s.offset = staging_bytes;
        sections.push_back(s);
        staging_bytes = align_up(staging_bytes + s.size_bytes);
    }
    for (PendingTexture& texture : pending) {
        for (const Image& level : texture.pixels.levels) {
            texture.level_offsets.push_back(staging_bytes);
            const VkDeviceSize texels = VkDeviceSize{static_cast<std::uint32_t>(level.width())} *
                                        static_cast<std::uint32_t>(level.height());
            staging_bytes = align_up(staging_bytes + texels * texel_bytes);
        }
    }
    if (staging_bytes > detail::max_buffer_bytes(device))
        throw std::runtime_error(std::string(who) + ": the upload takes " +
                                 std::to_string(staging_bytes) +
                                 " bytes, more than the device can allocate as one buffer");
    if (staging_bytes == 0)
        return; // an empty world: nothing to allocate or upload

    try {
        for (const Section& s : sections)
            create_buffer(s.buffer, s.size_bytes);
        texture_allocations_.reserve(textures_.size());
        for (Texture& texture : textures_)
            create_image(texture);

        // The staging buffer, filled; textures become half floats here, and their float levels
        // are freed as soon as they are staged.
        const detail::HostBuffer staging(device.allocator(), staging_bytes,
                                         detail::HostBuffer::Use::upload);
        for (const Section& s : sections)
            std::memcpy(staging.data() + s.offset, s.data, static_cast<std::size_t>(s.size_bytes));
        for (PendingTexture& texture : pending) {
            for (std::size_t level = 0; level < texture.pixels.levels.size(); ++level)
                write_half_floats(texture.pixels.levels[level],
                                  staging.data() + texture.level_offsets[level]);
            texture.pixels = {};
        }
        staging.flush();

        detail::OneTimeCommands commands(device); // destroyed before staging, after any late finish
        record_upload(commands.begin(), staging.buffer(), sections, buffers_, textures_, pending);
        commands.submit_and_wait();
    } catch (...) {
        destroy();
        throw;
    }
}

void Scene::create_buffer(SceneBufferKind kind, VkDeviceSize size_bytes) {
    const auto b = static_cast<std::size_t>(kind);
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size_bytes;
    info.usage = buffer_usage;
    if (kind == SceneBufferKind::indices)
        info.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    SceneBuffer& buffer = buffers_[b];
    detail::check(vmaCreateBuffer(device_->allocator(), &info, &allocation, &buffer.buffer,
                                  &buffer_allocations_[b], nullptr),
                  "allocating a buffer", who);
    buffer.size_bytes = size_bytes;
    VkBufferDeviceAddressInfo address{};
    address.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    address.buffer = buffer.buffer;
    buffer.address = vkGetBufferDeviceAddress(device_->device(), &address);
}

void Scene::create_image(Texture& texture) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = texture_format;
    info.extent = {texture.width, texture.height, 1};
    info.mipLevels = texture.mip_levels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = texture_usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    // Reserved by the constructor, so this cannot throw: destroy() pairs each allocation with its
    // texture by index.
    VmaAllocation& image_allocation = texture_allocations_.emplace_back(VK_NULL_HANDLE);
    detail::check(vmaCreateImage(device_->allocator(), &info, &allocation, &texture.image,
                                 &image_allocation, nullptr),
                  "allocating a texture", who);
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = texture.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = texture_format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, texture.mip_levels, 0, 1};
    detail::check(vkCreateImageView(device_->device(), &view, nullptr, &texture.view),
                  "creating a texture view", who);
}

Scene::~Scene() {
    destroy();
}

void Scene::destroy() noexcept {
    const VmaAllocator allocator = device_->allocator();
    for (std::size_t t = 0; t < textures_.size(); ++t) {
        vkDestroyImageView(device_->device(), textures_[t].view, nullptr);
        if (t < texture_allocations_.size()) // images not yet made have no allocation
            vmaDestroyImage(allocator, textures_[t].image, texture_allocations_[t]);
    }
    textures_.clear();
    texture_allocations_.clear();
    for (std::size_t b = 0; b < buffer_count; ++b)
        vmaDestroyBuffer(allocator, buffers_[b].buffer, buffer_allocations_[b]);
    buffers_ = {};
    buffer_allocations_ = {};
}

VkDeviceSize Scene::memory_bytes() const {
    VkDeviceSize bytes = 0;
    const auto add = [&](VmaAllocation allocation) {
        if (allocation == VK_NULL_HANDLE)
            return;
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(device_->allocator(), allocation, &info);
        bytes += info.size;
    };
    std::ranges::for_each(buffer_allocations_, add);
    std::ranges::for_each(texture_allocations_, add);
    return bytes;
}

std::vector<std::byte> Scene::download(SceneBufferKind which) const {
    const SceneBuffer& source = buffer(which);
    if (source.size_bytes == 0)
        return {};
    const detail::HostBuffer readback(device_->allocator(), source.size_bytes,
                                      detail::HostBuffer::Use::download);
    detail::OneTimeCommands commands(*device_); // destroyed first, after any late finish
    const VkCommandBuffer cmd = commands.begin();
    // The upload's barrier already made the buffer's contents visible to this copy.
    const VkBufferCopy copy{0, 0, source.size_bytes};
    vkCmdCopyBuffer(cmd, source.buffer, readback.buffer(), 1, &copy);
    detail::record_host_read(cmd, readback);
    commands.submit_and_wait();
    readback.invalidate();
    std::vector<std::byte> bytes(static_cast<std::size_t>(source.size_bytes));
    std::memcpy(bytes.data(), readback.data(), bytes.size());
    return bytes;
}

Image Scene::download_texture(std::uint32_t texture, std::uint32_t level) const {
    if (texture >= textures_.size())
        throw std::out_of_range(std::string(who) + ": there is no texture " +
                                std::to_string(texture));
    const Texture& source = textures_[texture];
    if (level >= source.mip_levels)
        throw std::out_of_range(std::string(who) + ": texture " + std::to_string(texture) +
                                " has no mip level " + std::to_string(level));
    const std::uint32_t width = level_side(source.width, level);
    const std::uint32_t height = level_side(source.height, level);
    const detail::HostBuffer readback(device_->allocator(),
                                      VkDeviceSize{width} * height * texel_bytes,
                                      detail::HostBuffer::Use::download);
    detail::OneTimeCommands commands(*device_);
    const VkCommandBuffer cmd = commands.begin();

    // Out of the shaders' layout for the copy, and back. Earlier commands only read the texture
    // (the upload's barrier made its writes visible), and the copy only reads it, so the
    // transitions wait for those reads without flushing anything.
    using detail::Access;
    const Access copy_read{VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
    const VkImageMemoryBarrier2 to_copy = detail::image_barrier(
        source.image, level, 1, {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE}, copy_read,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    detail::record_barriers(cmd, {}, {&to_copy, 1});

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer(), 1, &region);

    const VkImageMemoryBarrier2 to_read = detail::image_barrier(
        source.image, level, 1, {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_NONE},
        {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT},
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    detail::record_barriers(cmd, {}, {&to_read, 1});
    detail::record_host_read(cmd, readback);
    commands.submit_and_wait();

    readback.invalidate();
    Image image(static_cast<int>(width), static_cast<int>(height));
    const std::byte* in = readback.data();
    std::uint16_t texel[texture_channels];
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x, in += sizeof texel) {
            std::memcpy(texel, in, sizeof texel);
            image.at(x, y) = {glm::unpackHalf1x16(texel[0]), glm::unpackHalf1x16(texel[1]),
                              glm::unpackHalf1x16(texel[2])};
        }
    }
    return image;
}

} // namespace cs::gpu
