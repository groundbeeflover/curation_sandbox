// SPDX-License-Identifier: Apache-2.0

#include "cs/gpu/scene.hpp"

#include "upload.hpp"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <map>
#include <set>
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

// ---- The world, checked ------------------------------------------------------------------------

void validate(const world::World& world) {
    for (std::size_t m = 0; m < world.meshes.size(); ++m) {
        const geom::Mesh& mesh = world.meshes[m];
        const std::string name = "mesh " + std::to_string(m);
        if (mesh.normals.size() != mesh.positions.size() ||
            mesh.uvs.size() != mesh.positions.size())
            throw std::invalid_argument(std::string(who) + ": " + name +
                                        " has not one normal and one uv per position");
        if (mesh.indices.size() % 3 != 0)
            throw std::invalid_argument(std::string(who) + ": " + name +
                                        " has a number of indices that is not a multiple of 3");
        for (const std::uint32_t index : mesh.indices) {
            if (index >= mesh.positions.size())
                throw std::invalid_argument(std::string(who) + ": " + name + " has index " +
                                            std::to_string(index) + ", out of range");
        }
    }
    for (std::size_t i = 0; i < world.instances.size(); ++i) {
        const world::Instance& instance = world.instances[i];
        if (instance.mesh >= world.meshes.size())
            throw std::invalid_argument(std::string(who) + ": instance " + std::to_string(i) +
                                        " names mesh " + std::to_string(instance.mesh) +
                                        ", which is not in the world");
        if (instance.material >= world.materials.size())
            throw std::invalid_argument(std::string(who) + ": instance " + std::to_string(i) +
                                        " names material " + std::to_string(instance.material) +
                                        ", which is not in the world");
    }
}

// ---- Textures, checked -------------------------------------------------------------------------

// Why the device cannot hold this texture as given, or empty when it can. The loader is anyone's
// function, so its chain is checked as load_texture promises it: each level half the one above,
// rounded down and at least 1, down to 1 x 1.
std::string texture_problem(const assets::Texture& texture, VkPhysicalDevice physical_device) {
    if (texture.levels.empty() || texture.width() <= 0 || texture.height() <= 0)
        return "the image is empty";
    const auto width = static_cast<std::uint32_t>(texture.width());
    const auto height = static_cast<std::uint32_t>(texture.height());
    // Exactly the full chain: more levels would pass the checks below (each 1 x 1 level is half
    // of 1 x 1, rounded up), and the image would be made with more levels than its size allows.
    if (texture.levels.size() != static_cast<std::size_t>(std::bit_width(std::max(width, height))))
        return "the mip chain does not have one level for each halving down to 1 x 1";
    const auto levels = static_cast<std::uint32_t>(texture.levels.size());
    for (std::uint32_t level = 0; level < levels; ++level) {
        const Image& image = texture.levels[level];
        if (static_cast<std::uint32_t>(image.width()) != level_side(width, level) ||
            static_cast<std::uint32_t>(image.height()) != level_side(height, level))
            return "mip level " + std::to_string(level) + " is not half the level above";
    }
    VkImageFormatProperties properties{};
    if (vkGetPhysicalDeviceImageFormatProperties(physical_device, texture_format, VK_IMAGE_TYPE_2D,
                                                 VK_IMAGE_TILING_OPTIMAL, texture_usage, 0,
                                                 &properties) != VK_SUCCESS)
        return "the device cannot sample RGBA16F textures";
    VkDeviceSize bytes = 0;
    for (std::uint32_t level = 0; level < levels; ++level)
        bytes += VkDeviceSize{level_side(width, level)} * level_side(height, level) * texel_bytes;
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

// ---- Device objects ----------------------------------------------------------------------------

template <typename T> const std::byte* bytes_of(const std::vector<T>& values) {
    static_assert(std::is_trivially_copyable_v<T>);
    return reinterpret_cast<const std::byte*>(values.data());
}

template <typename T> VkDeviceSize size_of(const std::vector<T>& values) {
    return VkDeviceSize{values.size()} * sizeof(T);
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

    // Totals first: vertexOffset is an int32_t, first_index a uint32_t.
    std::uint64_t vertex_total = 0;
    std::uint64_t index_total = 0;
    for (const geom::Mesh& mesh : world.meshes) {
        vertex_total += mesh.positions.size();
        index_total += mesh.indices.size();
    }
    if (vertex_total > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) ||
        index_total > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error(std::string(who) + ": the world's meshes have " +
                                std::to_string(vertex_total) + " vertices and " +
                                std::to_string(index_total) +
                                " indices, more than 32-bit draws can count");

    // ---- What the buffers hold, on the host.
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(static_cast<std::size_t>(vertex_total));
    indices.reserve(static_cast<std::size_t>(index_total));
    meshes_.reserve(world.meshes.size());
    for (const geom::Mesh& mesh : world.meshes) {
        MeshRange range;
        range.first_vertex = static_cast<std::uint32_t>(vertices.size());
        range.vertex_count = static_cast<std::uint32_t>(mesh.positions.size());
        range.first_index = static_cast<std::uint32_t>(indices.size());
        range.index_count = static_cast<std::uint32_t>(mesh.indices.size());
        meshes_.push_back(range);
        for (std::size_t v = 0; v < mesh.positions.size(); ++v)
            vertices.push_back(
                {glm::vec3(mesh.positions[v]), glm::vec3(mesh.normals[v]), glm::vec2(mesh.uvs[v])});
        indices.insert(indices.end(), mesh.indices.begin(), mesh.indices.end());
    }

    instance_data_.reserve(world.instances.size());
    for (const world::Instance& instance : world.instances) {
        const MeshRange& range = meshes_[instance.mesh];
        InstanceData data;
        data.transform = glm::mat4(instance.transform);
        data.mesh = instance.mesh;
        data.material = instance.material;
        data.first_index = range.first_index;
        data.vertex_offset = range.first_vertex;
        instance_data_.push_back(data);
    }

    // ---- Materials, and the textures they name: each asset loaded once.
    std::vector<assets::Texture> pixels;             // parallel to textures_ until it is staged
    std::map<std::string, std::uint32_t> texture_of; // "<sha256>.<extension>" -> textures_
    std::set<std::string> failed;                    // assets that gave a problem
    const auto texture_for = [&](const world::AssetRef& asset) -> std::uint32_t {
        const std::string key = asset.sha256 + "." + asset.extension;
        if (const auto found = texture_of.find(key); found != texture_of.end())
            return found->second;
        if (failed.contains(key))
            return no_texture;
        std::string reason;
        try {
            assets::Texture texture = load_texture(asset);
            reason = texture_problem(texture, device.physical_device());
            if (reason.empty()) {
                const auto index = static_cast<std::uint32_t>(textures_.size());
                Texture entry;
                entry.asset = asset;
                entry.width = static_cast<std::uint32_t>(texture.width());
                entry.height = static_cast<std::uint32_t>(texture.height());
                entry.mip_levels = static_cast<std::uint32_t>(texture.levels.size());
                textures_.push_back(std::move(entry));
                pixels.push_back(std::move(texture));
                texture_of.emplace(key, index);
                return index;
            }
        } catch (const std::runtime_error& e) {
            reason = e.what();
        }
        failed.insert(key);
        problems_.push_back({asset, reason});
        return no_texture;
    };
    material_data_.reserve(world.materials.size());
    for (const world::Material& material : world.materials) {
        MaterialData data;
        if (const auto* lambert = std::get_if<world::LambertMaterial>(&material)) {
            data.kind = MaterialKind::lambert;
            data.base_colour = glm::vec3(lambert->albedo);
        } else {
            const auto& painting = std::get<world::PaintingSurface>(material);
            const material::PaintingMaterial& p = painting.parameters;
            data.kind = MaterialKind::painting;
            data.base_colour = glm::vec3(p.base.color);
            data.roughness = static_cast<float>(p.base.roughness);
            data.specular_weight = static_cast<float>(p.base.specular_weight);
            data.coat_weight = static_cast<float>(p.coat.weight);
            data.coat_roughness = static_cast<float>(p.coat.roughness);
            data.coat_ior = static_cast<float>(p.coat.ior);
            if (painting.image)
                data.texture = texture_for(*painting.image);
        }
        material_data_.push_back(data);
    }

    // ---- The staging buffer's layout: the four buffers, then every level of every texture.
    const std::array<std::pair<const std::byte*, VkDeviceSize>, buffer_count> sources = {{
        {bytes_of(vertices), size_of(vertices)},
        {bytes_of(indices), size_of(indices)},
        {bytes_of(instance_data_), size_of(instance_data_)},
        {bytes_of(material_data_), size_of(material_data_)},
    }};
    std::array<VkDeviceSize, buffer_count> buffer_offsets{};
    VkDeviceSize staging_bytes = 0;
    for (std::size_t b = 0; b < buffer_count; ++b) {
        buffer_offsets[b] = staging_bytes;
        staging_bytes = align_up(staging_bytes + sources[b].second);
    }
    std::vector<std::vector<VkDeviceSize>> level_offsets(textures_.size());
    for (std::size_t t = 0; t < textures_.size(); ++t) {
        for (const Image& level : pixels[t].levels) {
            level_offsets[t].push_back(staging_bytes);
            const VkDeviceSize level_pixels =
                VkDeviceSize{static_cast<std::uint32_t>(level.width())} *
                static_cast<std::uint32_t>(level.height());
            staging_bytes = align_up(staging_bytes + level_pixels * texel_bytes);
        }
    }
    if (staging_bytes > detail::max_buffer_bytes(device))
        throw std::runtime_error(std::string(who) + ": the upload takes " +
                                 std::to_string(staging_bytes) +
                                 " bytes, more than the device can allocate as one buffer");

    const VmaAllocator allocator = device.allocator();
    try {
        // ---- Device objects.
        for (std::size_t b = 0; b < buffer_count; ++b) {
            if (sources[b].second == 0)
                continue;
            VkBufferCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            info.size = sources[b].second;
            info.usage = buffer_usage;
            if (static_cast<SceneBufferKind>(b) == SceneBufferKind::indices)
                info.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VmaAllocationCreateInfo allocation{};
            allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            SceneBuffer& buffer = buffers_[b];
            detail::check(vmaCreateBuffer(allocator, &info, &allocation, &buffer.buffer,
                                          &buffer_allocations_[b], nullptr),
                          who, "allocating a buffer");
            buffer.size_bytes = info.size;
            VkBufferDeviceAddressInfo address{};
            address.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
            address.buffer = buffer.buffer;
            buffer.address = vkGetBufferDeviceAddress(device.device(), &address);
        }
        texture_allocations_.reserve(textures_.size());
        for (Texture& texture : textures_) {
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
            VmaAllocation& image_allocation = texture_allocations_.emplace_back(VK_NULL_HANDLE);
            detail::check(vmaCreateImage(allocator, &info, &allocation, &texture.image,
                                         &image_allocation, nullptr),
                          who, "allocating a texture");
            VkImageViewCreateInfo view{};
            view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image = texture.image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = texture_format;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, texture.mip_levels, 0, 1};
            detail::check(vkCreateImageView(device.device(), &view, nullptr, &texture.view), who,
                          "creating a texture view");
        }
        if (staging_bytes == 0)
            return; // an empty world: nothing to upload

        // ---- The staging buffer, filled; textures become half floats here.
        const detail::HostBuffer staging(allocator, staging_bytes, detail::HostBuffer::Use::upload);
        for (std::size_t b = 0; b < buffer_count; ++b) {
            if (sources[b].second != 0)
                std::memcpy(staging.data() + buffer_offsets[b], sources[b].first,
                            static_cast<std::size_t>(sources[b].second));
        }
        for (std::size_t t = 0; t < textures_.size(); ++t) {
            for (std::size_t level = 0; level < pixels[t].levels.size(); ++level) {
                std::uint16_t texel[texture_channels];
                std::byte* out = staging.data() + level_offsets[t][level];
                const Image& image = pixels[t].levels[level];
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
            pixels[t] = {}; // staged: the float copy is not needed any more
        }
        staging.flush();

        // ---- One submission: every copy, between the barriers that order it.
        detail::OneTimeCommands commands(device);
        const VkCommandBuffer cmd = commands.begin();

        std::vector<VkImageMemoryBarrier2> to_copy;
        for (const Texture& texture : textures_) {
            VkImageMemoryBarrier2& barrier =
                to_copy.emplace_back(detail::image_barrier(texture.image, 0, texture.mip_levels));
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE; // a new image: nothing to wait for
            barrier.srcAccessMask = VK_ACCESS_2_NONE;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        }
        if (!to_copy.empty())
            detail::record_barriers(cmd, nullptr, nullptr, to_copy.data(),
                                    static_cast<std::uint32_t>(to_copy.size()));

        for (std::size_t b = 0; b < buffer_count; ++b) {
            if (sources[b].second == 0)
                continue;
            const VkBufferCopy copy{buffer_offsets[b], 0, sources[b].second};
            vkCmdCopyBuffer(cmd, staging.buffer(), buffers_[b].buffer, 1, &copy);
        }
        for (std::size_t t = 0; t < textures_.size(); ++t) {
            const Texture& texture = textures_[t];
            std::vector<VkBufferImageCopy> regions;
            for (std::uint32_t level = 0; level < texture.mip_levels; ++level) {
                VkBufferImageCopy& region = regions.emplace_back();
                region.bufferOffset = level_offsets[t][level]; // tightly packed rows
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                region.imageExtent = {level_side(texture.width, level),
                                      level_side(texture.height, level), 1};
            }
            vkCmdCopyBufferToImage(cmd, staging.buffer(), texture.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   static_cast<std::uint32_t>(regions.size()), regions.data());
        }

        // The copies' writes, visible to every later read on the queue, in this submission's
        // order or a later one's: the tiers' shaders and index fetches, and download()'s copies.
        VkMemoryBarrier2 written{};
        written.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        written.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        written.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        written.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        written.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        std::vector<VkImageMemoryBarrier2> to_read;
        for (const Texture& texture : textures_) {
            VkImageMemoryBarrier2& barrier =
                to_read.emplace_back(detail::image_barrier(texture.image, 0, texture.mip_levels));
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        detail::record_barriers(cmd, &written, nullptr, to_read.data(),
                                static_cast<std::uint32_t>(to_read.size()));
        commands.submit_and_wait();
    } catch (...) {
        destroy();
        throw;
    }
}

Scene::~Scene() {
    destroy();
}

void Scene::destroy() noexcept {
    const VmaAllocator allocator = device_->allocator();
    for (std::size_t t = 0; t < textures_.size(); ++t) {
        vkDestroyImageView(device_->device(), textures_[t].view, nullptr);
        if (t < texture_allocations_.size())
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
    VkBufferMemoryBarrier2 to_host = detail::buffer_barrier(readback.buffer());
    to_host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    detail::record_barriers(cmd, nullptr, &to_host, nullptr, 0);
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
    // (the upload's barrier made its writes visible), so the transitions wait for them without
    // flushing anything.
    VkImageMemoryBarrier2 to_copy = detail::image_barrier(source.image, level, 1);
    to_copy.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    to_copy.srcAccessMask = VK_ACCESS_2_NONE;
    to_copy.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_copy.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    to_copy.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    detail::record_barriers(cmd, nullptr, nullptr, &to_copy, 1);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer(), 1, &region);

    VkImageMemoryBarrier2 to_read = detail::image_barrier(source.image, level, 1);
    to_read.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_read.srcAccessMask = VK_ACCESS_2_NONE; // the copy only read it
    to_read.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    to_read.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkBufferMemoryBarrier2 to_host = detail::buffer_barrier(readback.buffer());
    to_host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    detail::record_barriers(cmd, nullptr, &to_host, &to_read, 1);
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

// ---- Memory ------------------------------------------------------------------------------------

MemoryInUse memory_in_use(const Device& device) {
    VmaTotalStatistics statistics{};
    vmaCalculateStatistics(device.allocator(), &statistics);
    MemoryInUse in_use;
    in_use.allocations = statistics.total.statistics.allocationCount;
    in_use.allocation_bytes = statistics.total.statistics.allocationBytes;
    in_use.block_bytes = statistics.total.statistics.blockBytes;
    return in_use;
}

} // namespace cs::gpu
