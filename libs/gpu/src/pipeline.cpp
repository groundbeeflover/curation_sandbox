// SPDX-License-Identifier: Apache-2.0

#include "cs/gpu/pipeline.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

namespace cs::gpu {

namespace {

// SPIR-V: a five-word header, then instructions whose first word is (word count << 16) | opcode.
constexpr std::size_t spirv_header_words = 5;
constexpr std::uint32_t op_entry_point = 15;
constexpr std::uint32_t execution_model_vertex = 0;
constexpr std::uint32_t execution_model_fragment = 4;

constexpr VkShaderStageFlags push_constant_stages =
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

// Whether the module has an entry point of this execution model and name. Checked before the
// pipeline is made, because a missing one is undefined behaviour without the validation layer.
bool has_entry_point(std::span<const std::uint32_t> spirv, std::uint32_t execution_model,
                     std::string_view name) {
    std::size_t at = spirv_header_words;
    while (at < spirv.size()) {
        const std::uint32_t word_count = spirv[at] >> 16;
        const std::uint32_t opcode = spirv[at] & 0xffffu;
        if (word_count == 0 || word_count > spirv.size() - at)
            return false; // malformed: stop rather than read past the end
        // OpEntryPoint: execution model, function id, then the name as a nul-terminated string
        // packed four bytes to a word, then the interface ids.
        if (opcode == op_entry_point && word_count >= 4 && spirv[at + 1] == execution_model) {
            const std::size_t bytes = (word_count - 3) * sizeof(std::uint32_t);
            std::string text(bytes, '\0');
            std::memcpy(text.data(), &spirv[at + 3], bytes);
            if (std::string_view(text.c_str()) == name)
                return true;
        }
        at += word_count;
    }
    return false;
}

std::string result_text(VkResult result) {
    return std::to_string(static_cast<int>(result));
}

} // namespace

std::span<const std::uint32_t> shader_spirv(std::string_view name) {
    const std::span<const EmbeddedShader> shaders = embedded_shaders();
    const auto found = std::ranges::find(shaders, name, &EmbeddedShader::name);
    if (found == shaders.end())
        throw std::out_of_range("no shader named \"" + std::string(name) +
                                "\" is embedded (libs/gpu/shaders/<name>.slang)");
    return found->spirv;
}

GraphicsPipeline::GraphicsPipeline(const Device& device, const GraphicsPipelineOptions& options)
    : device_(device.device()), push_constant_bytes_(options.push_constant_bytes) {
    const std::span<const std::uint32_t> spirv = shader_spirv(options.shader);
    if (!has_entry_point(spirv, execution_model_vertex, options.vertex_entry))
        throw std::invalid_argument("shader \"" + options.shader +
                                    "\" has no vertex entry point \"" + options.vertex_entry +
                                    "\"");
    if (!has_entry_point(spirv, execution_model_fragment, options.fragment_entry))
        throw std::invalid_argument("shader \"" + options.shader +
                                    "\" has no fragment entry point \"" + options.fragment_entry +
                                    "\"");
    if (options.colour_format == VK_FORMAT_UNDEFINED)
        throw std::invalid_argument("GraphicsPipeline: no colour format");
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.physical_device(), &properties);
    const std::uint32_t max_push_bytes = properties.limits.maxPushConstantsSize;
    if (push_constant_bytes_ % 4 != 0 || push_constant_bytes_ > max_push_bytes)
        throw std::invalid_argument(
            "GraphicsPipeline: push_constant_bytes is " + std::to_string(push_constant_bytes_) +
            "; it must be a multiple of 4 and at most " + std::to_string(max_push_bytes));

    VkShaderModule module = VK_NULL_HANDLE;
    try {
        VkShaderModuleCreateInfo module_info{};
        module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module_info.codeSize = spirv.size_bytes();
        module_info.pCode = spirv.data();
        if (const VkResult result = vkCreateShaderModule(device_, &module_info, nullptr, &module);
            result != VK_SUCCESS)
            throw std::runtime_error("GraphicsPipeline: creating the shader module of \"" +
                                     options.shader + "\" failed (" + result_text(result) + ")");

        const VkPushConstantRange push_range{push_constant_stages, 0, push_constant_bytes_};
        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.pushConstantRangeCount = push_constant_bytes_ > 0 ? 1 : 0;
        layout_info.pPushConstantRanges = &push_range;
        if (const VkResult result =
                vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout_);
            result != VK_SUCCESS)
            throw std::runtime_error("GraphicsPipeline: creating the pipeline layout failed (" +
                                     result_text(result) + ")");

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = module;
        stages[0].pName = options.vertex_entry.c_str();
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = module;
        stages[1].pName = options.fragment_entry.c_str();

        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo input_assembly{};
        input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology = options.topology;

        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterization{};
        rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = options.cull_mode;
        rasterization.frontFace = options.front_face;
        rasterization.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        const bool depth = options.depth_format != VK_FORMAT_UNDEFINED && options.depth_test;
        VkPipelineDepthStencilStateCreateInfo depth_stencil{};
        depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil.depthTestEnable = depth ? VK_TRUE : VK_FALSE;
        depth_stencil.depthWriteEnable = depth ? VK_TRUE : VK_FALSE;
        depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;

        constexpr std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
        dynamic.pDynamicStates = dynamic_states.data();

        VkPipelineRenderingCreateInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &options.colour_format;
        rendering.depthAttachmentFormat = options.depth_format;

        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.pNext = &rendering;
        pipeline_info.stageCount = static_cast<std::uint32_t>(stages.size());
        pipeline_info.pStages = stages.data();
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pViewportState = &viewport;
        pipeline_info.pRasterizationState = &rasterization;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pDepthStencilState = &depth_stencil;
        pipeline_info.pColorBlendState = &blend;
        pipeline_info.pDynamicState = &dynamic;
        pipeline_info.layout = layout_;
        if (const VkResult result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1,
                                                              &pipeline_info, nullptr, &pipeline_);
            result != VK_SUCCESS)
            throw std::runtime_error("GraphicsPipeline: creating the pipeline for \"" +
                                     options.shader + "\" failed (" + result_text(result) + ")");
    } catch (...) {
        // The module is needed only while the pipeline is created.
        vkDestroyShaderModule(device_, module, nullptr);
        destroy();
        throw;
    }
    vkDestroyShaderModule(device_, module, nullptr);
}

GraphicsPipeline::~GraphicsPipeline() {
    destroy();
}

void GraphicsPipeline::destroy() noexcept {
    vkDestroyPipeline(device_, pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, layout_, nullptr);
}

void GraphicsPipeline::bind(VkCommandBuffer commands) const {
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
}

void GraphicsPipeline::push_constants(VkCommandBuffer commands,
                                      std::span<const std::byte> bytes) const {
    if (bytes.empty() || bytes.size() % 4 != 0 || bytes.size() > push_constant_bytes_)
        throw std::invalid_argument(
            "GraphicsPipeline::push_constants: " + std::to_string(bytes.size()) +
            " bytes; the range holds a multiple of 4 up to " +
            std::to_string(push_constant_bytes_));
    vkCmdPushConstants(commands, layout_, push_constant_stages, 0,
                       static_cast<std::uint32_t>(bytes.size()), bytes.data());
}

} // namespace cs::gpu
