// SPDX-License-Identifier: Apache-2.0

// The colour pipeline on the GPU (tonemap.hpp, issue #159, M1-44): colour.slang's exposure and
// PBR Neutral against libs/core's, and the tone-mapping pass against libs/core's CPU pipeline
// (exposure_scale, pbr_neutral, then linear_to_srgb8), from an RGBA32F image written here and
// from the Standard tier's.
//
// Every test that needs a device starts with make_device() (gpu_test.hpp): without a Vulkan
// device it skips, and with CS_REQUIRE_GPU=1 it fails. The harness (main.cpp) fails the running
// test on any validation warning or error, so every test here is also validation-clean. Images,
// tiers and passes are declared after the device, so they go before it.

#include "gpu_test.hpp"

#include <cs/assets/texture.hpp>
#include <cs/core/colour.hpp>
#include <cs/core/image.hpp>
#include <cs/geom/mesh.hpp>
#include <cs/gpu/device.hpp>
#include <cs/gpu/pipeline.hpp>
#include <cs/gpu/scene.hpp>
#include <cs/gpu/standard_tier.hpp>
#include <cs/gpu/tonemap.hpp>
#include <cs/light/distribution.hpp>
#include <cs/light/luminaire.hpp>
#include <cs/world/ids.hpp>
#include <cs/world/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using cs::gpu::Device;
using cs::gpu::Scene;
using cs::gpu::StandardImages;
using cs::gpu::StandardTier;
using cs::gpu::ToneMapPass;
using cs::gpu::test::Cleanup;
using cs::gpu::test::make_device;
using glm::dvec3;

namespace geom = cs::geom;
namespace light = cs::light;
namespace world = cs::world;

namespace {

constexpr double pi = std::numbers::pi;
constexpr float quiet_nan = std::numeric_limits<float>::quiet_NaN();

// The owner's comment on #159: the display image is R8G8B8A8_SRGB, encoded by its format.
static_assert(ToneMapPass::display_format == VK_FORMAT_R8G8B8A8_SRGB);

// ---- Commands, buffers and images --------------------------------------------------------------

// Records commands with `record` into a one-time command buffer, submits it to the device's
// queue and waits for it, then frees the pool and fence.
void submit_and_wait(const Device& gpu, const std::function<void(VkCommandBuffer)>& record) {
    const VkDevice device = gpu.device();
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    const Cleanup cleanup([&] {
        vkDestroyFence(device, fence, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
    });
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = gpu.queue_family();
    REQUIRE(vkCreateCommandPool(device, &pool_info, nullptr, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo command_info{};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    REQUIRE(vkAllocateCommandBuffers(device, &command_info, &commands) == VK_SUCCESS);
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    REQUIRE(vkCreateFence(device, &fence_info, nullptr, &fence) == VK_SUCCESS);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    REQUIRE(vkBeginCommandBuffer(commands, &begin) == VK_SUCCESS);
    record(commands);
    REQUIRE(vkEndCommandBuffer(commands) == VK_SUCCESS);

    VkCommandBufferSubmitInfo command_submit{};
    command_submit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    command_submit.commandBuffer = commands;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command_submit;
    REQUIRE(vkQueueSubmit2(gpu.queue(), 1, &submit, fence) == VK_SUCCESS);
    REQUIRE(vkWaitForFences(device, 1, &fence, VK_TRUE, 60'000'000'000ull) == VK_SUCCESS);
}

// Records one image barrier on the colour aspect of a single-level image.
void transition(VkCommandBuffer commands, VkImage image, VkPipelineStageFlags2 src_stages,
                VkAccessFlags2 src_accesses, VkPipelineStageFlags2 dst_stages,
                VkAccessFlags2 dst_accesses, VkImageLayout old_layout, VkImageLayout new_layout) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = src_stages;
    barrier.srcAccessMask = src_accesses;
    barrier.dstStageMask = dst_stages;
    barrier.dstAccessMask = dst_accesses;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commands, &dependency);
}

// A host-visible buffer, mapped, with a device address when its usage asks for one; freed with
// the object.
class MappedBuffer {
  public:
    MappedBuffer(const Device& device, VkDeviceSize size_bytes, VkBufferUsageFlags usage)
        : allocator_(device.allocator()) {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size_bytes;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo allocation_info{};
        allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
        allocation_info.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo mapped{};
        REQUIRE(vmaCreateBuffer(allocator_, &info, &allocation_info, &buffer_, &allocation_,
                                &mapped) == VK_SUCCESS);
        data_ = static_cast<std::byte*>(mapped.pMappedData);
        if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0) {
            VkBufferDeviceAddressInfo address_info{};
            address_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
            address_info.buffer = buffer_;
            address_ = vkGetBufferDeviceAddress(device.device(), &address_info);
        }
    }
    ~MappedBuffer() { vmaDestroyBuffer(allocator_, buffer_, allocation_); }
    MappedBuffer(const MappedBuffer&) = delete;
    MappedBuffer& operator=(const MappedBuffer&) = delete;

    VkBuffer buffer() const { return buffer_; }
    std::byte* data() const { return data_; }
    VkDeviceAddress address() const { return address_; }
    void flush() const {
        REQUIRE(vmaFlushAllocation(allocator_, allocation_, 0, VK_WHOLE_SIZE) == VK_SUCCESS);
    }
    void invalidate() const {
        REQUIRE(vmaInvalidateAllocation(allocator_, allocation_, 0, VK_WHOLE_SIZE) == VK_SUCCESS);
    }

  private:
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    std::byte* data_ = nullptr;
    VkDeviceAddress address_ = 0;
};

// A buffer the compute shader reads and writes through its device address.
MappedBuffer storage_buffer(const Device& gpu, VkDeviceSize size_bytes) {
    return {gpu, size_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
}

// An image of StandardTier::hdr_format holding these texels (RGB nits, A lux), width by height,
// row by row from the top, and left as StandardTier::record() leaves its HDR image: in
// SHADER_READ_ONLY_OPTIMAL, its writes visible to fragment and compute shader reads.
class HdrImage {
  public:
    HdrImage(const Device& gpu, std::uint32_t width, std::uint32_t height,
             std::span<const glm::vec4> texels)
        : device_(gpu.device()), allocator_(gpu.allocator()) {
        REQUIRE(texels.size() == std::size_t{width} * height);
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = StandardTier::hdr_format;
        info.extent = {width, height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo allocation_info{};
        allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        REQUIRE(vmaCreateImage(allocator_, &info, &allocation_info, &image_, &allocation_,
                               nullptr) == VK_SUCCESS);
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = image_;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = StandardTier::hdr_format;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        REQUIRE(vkCreateImageView(device_, &view_info, nullptr, &view_) == VK_SUCCESS);

        const VkDeviceSize bytes = texels.size_bytes();
        const MappedBuffer staging(gpu, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        std::memcpy(staging.data(), texels.data(), bytes);
        staging.flush();
        submit_and_wait(gpu, [&](VkCommandBuffer commands) {
            transition(commands, image_, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {width, height, 1};
            vkCmdCopyBufferToImage(commands, staging.buffer(), image_,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            transition(
                commands, image_, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
    }
    ~HdrImage() {
        vkDestroyImageView(device_, view_, nullptr);
        vmaDestroyImage(allocator_, image_, allocation_);
    }
    HdrImage(const HdrImage&) = delete;
    HdrImage& operator=(const HdrImage&) = delete;

    VkImageView view() const { return view_; }

  private:
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
};

// ---- colour.slang, through colour_check.slang ------------------------------------------------

// colour_check.slang's push constants, in C++ packing.
struct ColourCheckPush {
    VkDeviceAddress colours = 0;
    VkDeviceAddress tone_mapped = 0;
    VkDeviceAddress ev100 = 0;
    VkDeviceAddress scales = 0;
    std::uint32_t colour_count = 0;
    std::uint32_t ev100_count = 0;
};
static_assert(sizeof(ColourCheckPush) == 40);

struct ShaderColour {
    std::vector<glm::vec3> tone_mapped; // pbr_neutral of each colour
    std::vector<float> scales;          // exposure_scale of each EV100
};

// colour.slang's pbr_neutral of each colour and exposure_scale of each EV100, on the GPU.
ShaderColour run_colour_check(const Device& gpu, std::span<const glm::vec3> colours,
                              std::span<const float> ev100s) {
    const VkDevice device = gpu.device();
    // As float4s: the shader reads only scalars and float4s through device addresses.
    std::vector<glm::vec4> padded;
    for (const glm::vec3& c : colours) {
        padded.emplace_back(c, 0.0f);
    }
    const MappedBuffer inputs = storage_buffer(gpu, padded.size() * sizeof(glm::vec4));
    std::memcpy(inputs.data(), padded.data(), padded.size() * sizeof(glm::vec4));
    inputs.flush();
    const MappedBuffer outputs = storage_buffer(gpu, padded.size() * sizeof(glm::vec4));
    const MappedBuffer ev100_inputs = storage_buffer(gpu, ev100s.size_bytes());
    std::memcpy(ev100_inputs.data(), ev100s.data(), ev100s.size_bytes());
    ev100_inputs.flush();
    const MappedBuffer scale_outputs = storage_buffer(gpu, ev100s.size_bytes());

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const Cleanup cleanup([&] {
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyShaderModule(device, module, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);
    });
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ColourCheckPush)};
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &range;
    REQUIRE(vkCreatePipelineLayout(device, &layout_info, nullptr, &layout) == VK_SUCCESS);

    const std::span<const std::uint32_t> spirv = cs::gpu::shader_spirv("colour_check");
    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = spirv.size_bytes();
    module_info.pCode = spirv.data();
    REQUIRE(vkCreateShaderModule(device, &module_info, nullptr, &module) == VK_SUCCESS);

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = module;
    pipeline_info.stage.pName = "check_colour";
    pipeline_info.layout = layout;
    REQUIRE(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                     &pipeline) == VK_SUCCESS);

    ColourCheckPush push;
    push.colours = inputs.address();
    push.tone_mapped = outputs.address();
    push.ev100 = ev100_inputs.address();
    push.scales = scale_outputs.address();
    push.colour_count = static_cast<std::uint32_t>(colours.size());
    push.ev100_count = static_cast<std::uint32_t>(ev100s.size());
    const std::uint32_t threads = std::max(push.colour_count, push.ev100_count);
    submit_and_wait(gpu, [&](VkCommandBuffer commands) {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(commands, (threads + 63) / 64, 1, 1);
        // The shader's writes, then the host's reads after the fence.
        VkMemoryBarrier2 to_host{};
        to_host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        to_host.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        to_host.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &to_host;
        vkCmdPipelineBarrier2(commands, &dependency);
    });

    outputs.invalidate();
    scale_outputs.invalidate();
    ShaderColour result;
    for (std::size_t i = 0; i < colours.size(); ++i) {
        glm::vec4 out;
        std::memcpy(&out, outputs.data() + i * sizeof(glm::vec4), sizeof(out));
        result.tone_mapped.emplace_back(out);
    }
    result.scales.resize(ev100s.size());
    std::memcpy(result.scales.data(), scale_outputs.data(), ev100s.size_bytes());
    return result;
}

// libs/core/tests/test_colour.cpp's PBR Neutral case: its 10 inputs, repeated here as they are in
// an anonymous namespace there. The expected values are libs/core's pbr_neutral of them.
const std::array<dvec3, 10> pbr_neutral_inputs{{
    {0.0, 0.0, 0.0},
    {0.02, 0.03, 0.05},
    {0.1, 0.2, 0.3},
    {0.75, 0.6, 0.4},
    {0.82, 0.82, 0.82},
    {1.0, 0.0, 0.0},
    {0.0, 0.9, 0.2},
    {10.0, 10.0, 10.0},
    {100.0, 50.0, 20.0},
    {1.2, 0.7, 0.3},
}};

// ---- libs/core's CPU pipeline ------------------------------------------------------------------

// One pixel of linear RGB in nits as libs/core shows it at this EV100: exposed, tone mapped and
// encoded as 8-bit sRGB (cs/core/colour.hpp's three steps).
std::array<int, 3> cpu_display(dvec3 nits, double ev100) {
    const dvec3 display = cs::pbr_neutral(nits * cs::exposure_scale(ev100));
    return {cs::linear_to_srgb8(static_cast<float>(display.r)),
            cs::linear_to_srgb8(static_cast<float>(display.g)),
            cs::linear_to_srgb8(static_cast<float>(display.b))};
}

// The largest difference, in 8-bit steps, between the pass's pixels (RGBA, row by row) and
// libs/core's display of the radiance at each; and whether every alpha is 255.
struct Comparison {
    int worst = 0;
    int worst_x = 0;
    int worst_y = 0;
    bool opaque = true;
};

Comparison compare(std::span<const std::uint8_t> pixels, int width, int height, double ev100,
                   const std::function<dvec3(int, int)>& nits_at) {
    REQUIRE(pixels.size() == std::size_t{4} * width * height);
    Comparison result;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::uint8_t* gpu = &pixels[(std::size_t{4} * y * width) + (std::size_t{4} * x)];
            const std::array<int, 3> cpu = cpu_display(nits_at(x, y), ev100);
            for (int c = 0; c < 3; ++c) {
                const int difference = std::abs(int{gpu[c]} - cpu[c]);
                if (difference > result.worst) {
                    result = {difference, x, y, result.opaque};
                }
            }
            result.opaque = result.opaque && gpu[3] == 255;
        }
    }
    return result;
}

// ---- A world for the Standard tier -------------------------------------------------------------

// A 20 m Lambert floor at z = 0, of albedo 0.8, under an isotropic 1000 cd light 2 m above the
// origin (a uniform cone over the whole sphere, as standard_tier.hpp's tests make one).
world::World lit_floor() {
    geom::Mesh mesh;
    mesh.positions = {
        {-10.0, -10.0, 0.0}, {10.0, -10.0, 0.0}, {10.0, 10.0, 0.0}, {-10.0, 10.0, 0.0}};
    mesh.normals.assign(4, dvec3(0.0, 0.0, 1.0));
    mesh.uvs = {{0.0, 1.0}, {1.0, 1.0}, {1.0, 0.0}, {0.0, 0.0}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    world::Instance floor;
    floor.mesh = 0;
    floor.material = 0;
    const auto spot =
        std::make_shared<const light::GenericSpot>(4.0 * pi * 1000.0, 2.0 * pi, 2.0 * pi);
    world::World w;
    w.meshes = {mesh};
    w.materials = {world::LambertMaterial{dvec3(0.8)}};
    w.instances = {floor};
    w.lights = {world::Light{.entity = world::Id{},
                             .luminaire = light::Luminaire({0.0, 0.0, 2.0}, {0.0, 0.0, 1.0}, spot),
                             .spot = spot,
                             .colour_temperature_k = 3000.0}};
    return w;
}

// An orthographic camera 1 m above the origin, looking straight down, seeing 4 m vertically.
world::Camera looking_down() {
    world::Camera camera;
    camera.projection = world::Projection::orthographic;
    camera.position_m = {0.0, 0.0, 1.0};
    camera.forward = {0.0, 0.0, -1.0};
    camera.up = {0.0, 1.0, 0.0};
    camera.vertical_extent_m = 4.0;
    return camera;
}

// A loader for worlds without images: any call fails the test.
cs::gpu::TextureLoader no_loader() {
    return [](const world::AssetRef& ref) -> cs::assets::Texture {
        FAIL_CHECK("the loader was called for " << ref.sha256 << ", in a world without images");
        throw std::runtime_error("unexpected load");
    };
}

} // namespace

// ---- colour.slang ------------------------------------------------------------------------------

TEST_CASE("tonemap: colour.slang's pbr_neutral and exposure_scale match libs/core within 1e-4",
          "[gpu][tonemap][accuracy]") {
    const std::unique_ptr<Device> gpu = make_device();
    std::vector<glm::vec3> colours(pbr_neutral_inputs.begin(), pbr_neutral_inputs.end());
    // Negative and NaN channels count as 0, as in libs/core: each of these is (0, 0.5, 0.2) or
    // (0.9, 0, 3) to pbr_neutral.
    const std::array<std::pair<glm::vec3, dvec3>, 4> clamped{{
        {{-1.0f, 0.5f, 0.2f}, {0.0, 0.5, 0.2}},
        {{quiet_nan, 0.5f, 0.2f}, {0.0, 0.5, 0.2}},
        {{0.9f, -1e30f, 3.0f}, {0.9, 0.0, 3.0}},
        {{0.9f, quiet_nan, 3.0f}, {0.9, 0.0, 3.0}},
    }};
    for (const auto& c : clamped) {
        colours.push_back(c.first);
    }
    const std::array<float, 3> ev100s{6.0f, 8.0f, 10.0f};
    const ShaderColour shader = run_colour_check(*gpu, colours, ev100s);

    // 1e-4, the acceptance criterion's tolerance: PBR Neutral's outputs are in [0, 1], and float
    // arithmetic over its dozen operations costs a few times 1e-7 against libs/core's double.
    for (std::size_t i = 0; i < pbr_neutral_inputs.size(); ++i) {
        const dvec3 input = pbr_neutral_inputs[i];
        INFO("input (" << input.r << ", " << input.g << ", " << input.b << ")");
        const dvec3 expected = cs::pbr_neutral(input);
        CHECK_THAT(shader.tone_mapped[i].r, WithinAbs(expected.r, 1e-4));
        CHECK_THAT(shader.tone_mapped[i].g, WithinAbs(expected.g, 1e-4));
        CHECK_THAT(shader.tone_mapped[i].b, WithinAbs(expected.b, 1e-4));
    }
    for (std::size_t k = 0; k < clamped.size(); ++k) {
        const glm::vec3 input = clamped[k].first;
        INFO("input (" << input.r << ", " << input.g << ", " << input.b << ")");
        const dvec3 expected = cs::pbr_neutral(clamped[k].second);
        const glm::vec3 out = shader.tone_mapped[pbr_neutral_inputs.size() + k];
        CHECK_THAT(out.r, WithinAbs(expected.r, 1e-4));
        CHECK_THAT(out.g, WithinAbs(expected.g, 1e-4));
        CHECK_THAT(out.b, WithinAbs(expected.b, 1e-4));
    }

    // Relative, as the scale is 1 / 1229 at EV100 10, where an absolute 1e-4 would pass a scale
    // off by 12%. The shader's exp2 and division are within a few ulp (Vulkan's precision rules:
    // exp2 within 3 + 2 |x| ulp), about 3e-6 at EV100 10.
    for (std::size_t k = 0; k < ev100s.size(); ++k) {
        INFO("EV100 " << ev100s[k]);
        CHECK_THAT(shader.scales[k], WithinRel(cs::exposure_scale(ev100s[k]), 1e-4));
    }
}

// ---- The pass ----------------------------------------------------------------------------------

TEST_CASE("tonemap: patches of known luminance, whatever the lux in A, match libs/core's CPU "
          "pipeline within 1/255 at EV100 6, 8 and 10, and are opaque",
          "[gpu][tonemap][accuracy]") {
    const std::unique_ptr<Device> gpu = make_device();
    // Columns: luminances in nits, from black to far beyond saturation at EV100 6 (76.8 nits), 8
    // (307 nits) and 10 (1229 nits). Rows: colours of luminance 1, grey, warm and cool white,
    // and Rec. 709's red, green and blue primaries. Outside the black column every pixel differs
    // from its neighbours, and the image is not square, so a texel read from the wrong pixel, or
    // transposed, shows.
    const std::array<double, 10> luminances{0.0,   0.5,   5.0,   20.0,   60.0,
                                            150.0, 300.0, 600.0, 1500.0, 1e4};
    const std::array<dvec3, 6> colours{
        dvec3(1.0),
        cs::cct_to_linear_rgb(2700.0),
        cs::cct_to_linear_rgb(6500.0),
        dvec3(1.0 / 0.2126, 0.0, 0.0),
        dvec3(0.0, 1.0 / 0.7152, 0.0),
        dvec3(0.0, 0.0, 1.0 / 0.0722),
    };
    // The lux in A, never alpha: 0 would make a pixel transparent, NaN poison it, a negative one
    // or 1e6 overflow a blend.
    const std::array<float, 5> lux{0.0f, 250.0f, 1e6f, -1.0f, quiet_nan};
    constexpr int width = static_cast<int>(luminances.size());
    constexpr int height = static_cast<int>(colours.size());
    const auto nits_at = [&](int x, int y) { return luminances[x] * colours[y]; };
    std::vector<glm::vec4> texels;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            texels.emplace_back(glm::vec3(nits_at(x, y)), lux[(x + y) % lux.size()]);
        }
    }
    const HdrImage hdr(*gpu, width, height, texels);
    const ToneMapPass pass(*gpu, hdr.view(), width, height);
    REQUIRE(pass.width() == std::uint32_t{width});
    REQUIRE(pass.height() == std::uint32_t{height});

    // 1/255, the acceptance criterion's tolerance: the shader computes in float and the hardware
    // rounds its sRGB encoding, so a value within a hair of a step's boundary may land one step
    // from libs/core's lround; anything more is a different curve.
    for (const double ev100 : {6.0, 8.0, 10.0}) {
        INFO("EV100 " << ev100);
        const std::vector<std::uint8_t> pixels = pass.render(ev100);
        const Comparison c = compare(pixels, width, height, ev100, nits_at);
        INFO("worst at (" << c.worst_x << ", " << c.worst_y << ")");
        CHECK(c.worst <= 1);
        CHECK(c.opaque);
    }
}

TEST_CASE("tonemap: the pass reads the Standard tier's HDR image as record() leaves it, frame "
          "after frame in one command buffer, and shows it as libs/core would",
          "[gpu][tonemap][accuracy]") {
    const std::unique_ptr<Device> gpu = make_device();
    const world::World w = lit_floor();
    const Scene scene(*gpu, w, no_loader());
    constexpr int width = 48;
    constexpr int height = 27;
    const StandardTier tier(*gpu, scene, w.lights, width, height);
    const ToneMapPass pass(*gpu, tier.hdr_view(), tier.width(), tier.height());
    const world::Camera camera = looking_down();
    // The floor under the light is 0.8 / pi * 250 lux = 64 nits: at EV100 6, beyond PBR Neutral's
    // knee (0.76 of 76.8 nits) in the middle, and below it towards the edges.
    constexpr double ev100 = 6.0;
    const StandardImages hdr = tier.render(camera);

    // No barrier between the tier and the pass, nor between frames: each says what it waits for.
    submit_and_wait(*gpu, [&](VkCommandBuffer commands) {
        tier.record(commands, camera);
        pass.record(commands, ev100);
        tier.record(commands, camera);
        pass.record(commands, ev100);
    });
    const std::vector<std::uint8_t> pixels = pass.render(ev100);
    const Comparison c = compare(pixels, width, height, ev100, [&](int x, int y) {
        const cs::Rgb& rgb = hdr.radiance.at(x, y);
        return dvec3(rgb.r, rgb.g, rgb.b);
    });
    INFO("worst at (" << c.worst_x << ", " << c.worst_y << ")");
    CHECK(c.worst <= 1);
    CHECK(c.opaque);
    // And the image spans the curve's knee: exposed, the centre's peak channel is beyond 0.76,
    // where PBR Neutral compresses, and the corner's well below it.
    const auto exposed_peak = [&](int x, int y) {
        const cs::Rgb& rgb = hdr.radiance.at(x, y);
        return std::max({rgb.r, rgb.g, rgb.b}) * cs::exposure_scale(ev100);
    };
    CHECK(exposed_peak(width / 2, height / 2) > 0.8);
    CHECK(exposed_peak(0, 0) < 0.5);

    // The display image is left in TRANSFER_SRC_OPTIMAL for the presenter's blit: a transition
    // from it is valid only if it is, which the validation layer checks at submission.
    submit_and_wait(*gpu, [&](VkCommandBuffer commands) {
        transition(commands, pass.display_image(), VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_NONE,
                   VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    });
}

TEST_CASE("tonemap: the pass refuses no HDR view, a zero or over-large size, and an EV100 that "
          "is not finite",
          "[gpu][tonemap]") {
    const std::unique_ptr<Device> gpu = make_device();
    const std::array<glm::vec4, 4> texels{};
    const HdrImage hdr(*gpu, 2, 2, texels);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gpu->physical_device(), &properties);
    const std::uint32_t too_large = properties.limits.maxImageDimension2D + 1;

    CHECK_THROWS_AS(ToneMapPass(*gpu, VK_NULL_HANDLE, 2, 2), std::invalid_argument);
    CHECK_THROWS_AS(ToneMapPass(*gpu, hdr.view(), 0, 2), std::invalid_argument);
    CHECK_THROWS_AS(ToneMapPass(*gpu, hdr.view(), 2, 0), std::invalid_argument);
    CHECK_THROWS_AS(ToneMapPass(*gpu, hdr.view(), too_large, 2), std::invalid_argument);
    CHECK_THROWS_AS(ToneMapPass(*gpu, hdr.view(), 2, too_large), std::invalid_argument);

    const ToneMapPass pass(*gpu, hdr.view(), 2, 2);
    for (const double ev100 :
         {static_cast<double>(quiet_nan), std::numeric_limits<double>::infinity(),
          -std::numeric_limits<double>::infinity()}) {
        INFO("EV100 " << ev100);
        CHECK_THROWS_AS((void)pass.render(ev100), std::invalid_argument);
    }
    // And the pass still works: black stays black.
    const std::vector<std::uint8_t> pixels = pass.render(8.0);
    CHECK(pixels ==
          std::vector<std::uint8_t>{0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255});
}
