// SPDX-License-Identifier: Apache-2.0

#include "cs/gpu/tonemap.hpp"

#include "cs/gpu/pipeline.hpp"
#include "upload.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace cs::gpu {

namespace {

using detail::check;

// tonemap.slang's push constants.
struct ToneMapPush {
    float ev100 = 0.0f;
};

constexpr VkDeviceSize display_texel_bytes = 4;

// Copies and blits of the display image: the readback here, the presenter's blit.
constexpr VkPipelineStageFlags2 transfer_reads =
    VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT;

} // namespace

ToneMapPass::ToneMapPass(const Device& device, VkImageView hdr_view, std::uint32_t width,
                         std::uint32_t height)
    : device_(&device), width_(width), height_(height) {
    if (hdr_view == VK_NULL_HANDLE)
        throw std::invalid_argument("ToneMapPass: no HDR image view");
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.physical_device(), &properties);
    const VkPhysicalDeviceLimits& limits = properties.limits;
    const std::uint32_t max_width =
        std::min(limits.maxImageDimension2D, limits.maxFramebufferWidth);
    const std::uint32_t max_height =
        std::min(limits.maxImageDimension2D, limits.maxFramebufferHeight);
    if (width == 0 || height == 0 || width > max_width || height > max_height)
        throw std::invalid_argument("ToneMapPass: " + std::to_string(width) + "x" +
                                    std::to_string(height) + " is not a size from 1x1 to " +
                                    std::to_string(max_width) + "x" + std::to_string(max_height));

    try {
        // Vulkan requires R8G8B8A8_SRGB to be a colour attachment and a transfer source on every
        // device, and at 4 bytes a texel this image is a quarter of the HDR image, which fit.
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = display_format;
        info.extent = {width, height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo allocation_info{};
        allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        check(vmaCreateImage(device.allocator(), &info, &allocation_info, &display_image_,
                             &display_allocation_, nullptr),
              "allocating the display image", "ToneMapPass");
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = display_image_;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = display_format;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device.device(), &view_info, nullptr, &display_view_),
              "creating the display image view", "ToneMapPass");

        create_descriptors(hdr_view);
        create_pipeline();
    } catch (...) {
        destroy();
        throw;
    }
}

ToneMapPass::~ToneMapPass() {
    destroy();
}

void ToneMapPass::destroy() noexcept {
    const VkDevice device = device_->device();
    vkDestroyPipeline(device, pipeline_, nullptr);
    vkDestroyPipelineLayout(device, layout_, nullptr);
    vkDestroyDescriptorPool(device, pool_, nullptr); // frees the set
    vkDestroyDescriptorSetLayout(device, set_layout_, nullptr);
    vkDestroyImageView(device, display_view_, nullptr);
    if (display_image_ != VK_NULL_HANDLE)
        vmaDestroyImage(device_->allocator(), display_image_, display_allocation_);
}

void ToneMapPass::create_descriptors(VkImageView hdr_view) {
    const VkDevice device = device_->device();
    // tonemap.slang: binding 0 the HDR image, read with Load, so without a sampler.
    const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                               VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 1;
    layout_info.pBindings = &binding;
    check(vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &set_layout_),
          "creating the descriptor set layout", "ToneMapPass");

    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1};
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &size;
    check(vkCreateDescriptorPool(device, &pool_info, nullptr, &pool_),
          "creating the descriptor pool", "ToneMapPass");

    VkDescriptorSetAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate_info.descriptorPool = pool_;
    allocate_info.descriptorSetCount = 1;
    allocate_info.pSetLayouts = &set_layout_;
    check(vkAllocateDescriptorSets(device, &allocate_info, &set_), "allocating the descriptor set",
          "ToneMapPass");

    const VkDescriptorImageInfo image{VK_NULL_HANDLE, hdr_view,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void ToneMapPass::create_pipeline() {
    const VkDevice device = device_->device();
    const VkPushConstantRange push_range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ToneMapPush)};
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    check(vkCreatePipelineLayout(device, &layout_info, nullptr, &layout_),
          "creating the pipeline layout", "ToneMapPass");

    const std::span<const std::uint32_t> spirv = shader_spirv("tonemap");
    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = spirv.size_bytes();
    module_info.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &module_info, nullptr, &module),
          "creating the shader module", "ToneMapPass");

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (VkPipelineShaderStageCreateInfo& stage : stages) {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.module = module;
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].pName = "vertex_main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].pName = "fragment_main";

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    // The size never changes, so the viewport and scissor are fixed: the whole image.
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_),
                              0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {width_, height_}};
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &display_format;

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.pNext = &rendering;
    pipeline_info.stageCount = static_cast<std::uint32_t>(stages.size());
    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.layout = layout_;
    const VkResult result =
        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_);
    // The module is needed only while the pipeline is made.
    vkDestroyShaderModule(device, module, nullptr);
    check(result, "creating the pipeline", "ToneMapPass");
}

void ToneMapPass::record(VkCommandBuffer commands, double ev100) const {
    // Also refuses NaN, which fails the comparison.
    if (!(std::abs(ev100) <= max_ev100_magnitude))
        throw std::invalid_argument("ToneMapPass: EV100 " + std::to_string(ev100) +
                                    " is not within plus or minus " +
                                    std::to_string(static_cast<int>(max_ev100_magnitude)));
    const ToneMapPush push{static_cast<float>(ev100)};

    // From UNDEFINED, discarding the last frame, once its copies and blits have read it.
    const VkImageMemoryBarrier2 to_attachment = detail::image_barrier(
        display_image_, 0, 1, {transfer_reads, VK_ACCESS_2_NONE},
        {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT},
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    detail::record_barriers(commands, {}, std::span(&to_attachment, 1));

    VkRenderingAttachmentInfo colour{};
    colour.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colour.imageView = display_view_;
    colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // the triangle covers every pixel
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {width_, height_}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &colour;
    vkCmdBeginRendering(commands, &rendering);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0,
                            nullptr);
    vkCmdPushConstants(commands, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(commands, 3, 1, 0, 0);
    vkCmdEndRendering(commands);

    const VkImageMemoryBarrier2 to_transfer = detail::image_barrier(
        display_image_, 0, 1,
        {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT},
        {transfer_reads, VK_ACCESS_2_TRANSFER_READ_BIT}, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    detail::record_barriers(commands, {}, std::span(&to_transfer, 1));
}

std::vector<std::uint8_t> ToneMapPass::render(double ev100) const {
    const VkDeviceSize bytes = VkDeviceSize{width_} * height_ * display_texel_bytes;
    detail::HostBuffer readback(device_->allocator(), bytes, detail::HostBuffer::Use::download);
    detail::OneTimeCommands commands(*device_);
    const VkCommandBuffer cmd = commands.begin();
    record(cmd, ev100);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width_, height_, 1}; // tightly packed rows
    vkCmdCopyImageToBuffer(cmd, display_image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer(), 1, &region);
    detail::record_host_read(cmd, readback);
    commands.submit_and_wait();
    readback.invalidate();

    std::vector<std::uint8_t> pixels(bytes);
    std::memcpy(pixels.data(), readback.data(), bytes);
    return pixels;
}

} // namespace cs::gpu
