// SPDX-License-Identifier: Apache-2.0
#pragma once

// Shaders and graphics pipelines.
//
// Every libs/gpu/shaders/*.slang is compiled to SPIR-V by the pinned slangc at build time
// (cmake/CsShaders.cmake) and embedded in the library, so no shader is compiled or read from a
// file at run time (ADR 0001), and a shader that does not compile fails the build. A shader is
// named by its file's stem ("triangle" for triangle.slang), and its entry points keep the names
// they have in the source: [shader("vertex")] vertex_main is "vertex_main".

#include <cs/gpu/device.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace cs::gpu {

// The first word of every SPIR-V module.
inline constexpr std::uint32_t spirv_magic = 0x07230203;

struct EmbeddedShader {
    std::string_view name;                // the .slang file's stem
    std::span<const std::uint32_t> spirv; // SPIR-V 1.6, starting with spirv_magic
};

// Every embedded shader, in name order.
[[nodiscard]] std::span<const EmbeddedShader> embedded_shaders();

// The SPIR-V of the shader with this name. Throws std::out_of_range, naming it, when there is
// none.
[[nodiscard]] std::span<const std::uint32_t> shader_spirv(std::string_view name);

struct GraphicsPipelineOptions {
    std::string shader; // an embedded shader's name
    std::string vertex_entry = "vertex_main";
    std::string fragment_entry = "fragment_main";
    // The formats of the attachments the pipeline renders into with dynamic rendering. With
    // depth_format VK_FORMAT_UNDEFINED there is no depth attachment, and no depth test.
    VkFormat colour_format = VK_FORMAT_UNDEFINED;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    // With a depth attachment: keep fragments nearer than the depth already there
    // (VK_COMPARE_OP_LESS) and write their depth.
    bool depth_test = true;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkCullModeFlags cull_mode = VK_CULL_MODE_NONE;
    VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    // One push-constant range, from offset 0, seen by the vertex and fragment stages. A multiple
    // of 4, at most the device's maxPushConstantsSize (128 on every device); 0 for none.
    std::uint32_t push_constant_bytes = 0;
};

// A graphics pipeline for dynamic rendering (no render pass), with one colour attachment and no
// blending. It has no vertex input: shaders read their vertices themselves, by SV_VertexID or
// through buffer device addresses in push constants. Viewport and scissor are dynamic state, set
// when recording (OffscreenTarget::render sets them to the whole target).
//
// Destroy it before its Device; it must not be in use by a pending command buffer when it goes.
class GraphicsPipeline {
  public:
    // Throws std::out_of_range (no such shader), std::invalid_argument (an entry point the
    // shader does not have, no colour format, a bad push-constant size) or std::runtime_error
    // (a Vulkan failure).
    GraphicsPipeline(const Device& device, const GraphicsPipelineOptions& options);
    ~GraphicsPipeline();

    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
    GraphicsPipeline(GraphicsPipeline&&) = delete;
    GraphicsPipeline& operator=(GraphicsPipeline&&) = delete;

    [[nodiscard]] VkPipeline pipeline() const { return pipeline_; }
    [[nodiscard]] VkPipelineLayout layout() const { return layout_; }
    [[nodiscard]] std::uint32_t push_constant_bytes() const { return push_constant_bytes_; }

    // vkCmdBindPipeline on the graphics bind point.
    void bind(VkCommandBuffer commands) const;
    // vkCmdPushConstants of these bytes, from offset 0, to the vertex and fragment stages. Throws
    // std::invalid_argument when they do not fit the range or are not a multiple of 4.
    void push_constants(VkCommandBuffer commands, std::span<const std::byte> bytes) const;
    // The same for one trivially copyable value, such as a struct that mirrors the shader's.
    template <typename T> void push_constants(VkCommandBuffer commands, const T& value) const {
        // Through a dynamic-extent span: as_bytes gives a fixed extent, which would pick this
        // template again rather than the overload above.
        const std::span<const std::byte> bytes = std::as_bytes(std::span<const T, 1>(&value, 1));
        push_constants(commands, bytes);
    }

  private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    std::uint32_t push_constant_bytes_ = 0;

    void destroy() noexcept;
};

} // namespace cs::gpu
