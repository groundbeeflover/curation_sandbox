// SPDX-License-Identifier: Apache-2.0
// Throwaway prototype for M1-04. A Vulkan 1.3 renderer that owns its instance, device and
// swapchain and knows nothing about Qt, as ADR 0006 asks of libs/gpu. Functions are loaded at
// run time (VK_NO_PROTOTYPES), the way volk will do it, so no import library is needed.
#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace spike {

#define SPIKE_VK_GLOBAL(X)                                                                         \
    X(vkCreateInstance)                                                                            \
    X(vkEnumerateInstanceExtensionProperties)                                                      \
    X(vkEnumerateInstanceLayerProperties)

#define SPIKE_VK_INSTANCE(X)                                                                       \
    X(vkDestroyInstance)                                                                           \
    X(vkEnumeratePhysicalDevices)                                                                  \
    X(vkGetPhysicalDeviceProperties)                                                               \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                    \
    X(vkGetPhysicalDeviceMemoryProperties)                                                         \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)                                                        \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                                   \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)                                                        \
    X(vkGetPhysicalDeviceFormatProperties)                                                         \
    X(vkCreateDevice)                                                                              \
    X(vkGetDeviceProcAddr)                                                                         \
    X(vkCreateDebugUtilsMessengerEXT)                                                              \
    X(vkDestroyDebugUtilsMessengerEXT)

#define SPIKE_VK_DEVICE(X)                                                                         \
    X(vkDestroyDevice)                                                                             \
    X(vkGetDeviceQueue)                                                                            \
    X(vkDeviceWaitIdle)                                                                            \
    X(vkCreateSwapchainKHR)                                                                        \
    X(vkDestroySwapchainKHR)                                                                       \
    X(vkGetSwapchainImagesKHR)                                                                     \
    X(vkAcquireNextImageKHR)                                                                       \
    X(vkQueuePresentKHR)                                                                           \
    X(vkQueueSubmit2)                                                                              \
    X(vkCreateImage)                                                                               \
    X(vkDestroyImage)                                                                              \
    X(vkCreateImageView)                                                                           \
    X(vkDestroyImageView)                                                                          \
    X(vkCreateBuffer)                                                                              \
    X(vkDestroyBuffer)                                                                             \
    X(vkGetImageMemoryRequirements)                                                                \
    X(vkGetBufferMemoryRequirements)                                                               \
    X(vkAllocateMemory)                                                                            \
    X(vkFreeMemory)                                                                                \
    X(vkBindImageMemory)                                                                           \
    X(vkBindBufferMemory)                                                                          \
    X(vkMapMemory)                                                                                 \
    X(vkUnmapMemory)                                                                               \
    X(vkCreateShaderModule)                                                                        \
    X(vkDestroyShaderModule)                                                                       \
    X(vkCreatePipelineLayout)                                                                      \
    X(vkDestroyPipelineLayout)                                                                     \
    X(vkCreateGraphicsPipelines)                                                                   \
    X(vkDestroyPipeline)                                                                           \
    X(vkCreateCommandPool)                                                                         \
    X(vkDestroyCommandPool)                                                                        \
    X(vkAllocateCommandBuffers)                                                                    \
    X(vkBeginCommandBuffer)                                                                        \
    X(vkEndCommandBuffer)                                                                          \
    X(vkResetCommandBuffer)                                                                        \
    X(vkCmdBeginRendering)                                                                         \
    X(vkCmdEndRendering)                                                                           \
    X(vkCmdPipelineBarrier2)                                                                       \
    X(vkCmdBindPipeline)                                                                           \
    X(vkCmdSetViewport)                                                                            \
    X(vkCmdSetScissor)                                                                             \
    X(vkCmdPushConstants)                                                                          \
    X(vkCmdDraw)                                                                                   \
    X(vkCmdCopyImageToBuffer)                                                                      \
    X(vkCreateSemaphore)                                                                           \
    X(vkDestroySemaphore)                                                                          \
    X(vkCreateFence)                                                                               \
    X(vkDestroyFence)                                                                              \
    X(vkWaitForFences)                                                                             \
    X(vkResetFences)

// An RGBA8 image, sRGB-encoded, top row first: what was last drawn into the viewport.
struct Readback {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

struct FrameParams {
    float angle_rad = 0.0f;
    float offset_y = 0.0f;
};

class VkRenderer {
  public:
    VkRenderer() = default;
    ~VkRenderer();
    VkRenderer(const VkRenderer&) = delete;
    VkRenderer& operator=(const VkRenderer&) = delete;

    // Loads the Vulkan loader and creates the instance with every surface extension the loader
    // offers (the UI decides later which one it uses), plus validation when asked and present.
    bool create_instance(bool validation);
    VkInstance instance() const { return instance_; }
    PFN_vkGetInstanceProcAddr get_instance_proc_addr() const { return get_instance_proc_addr_; }

    // Picks a device that can present to `surface`, or any device when `surface` is null
    // (headless: draws into an offscreen image). Does not take ownership of the surface.
    bool create_device(VkSurfaceKHR surface);

    // Sets the size of the target in pixels; the swapchain or offscreen image is recreated at
    // the next frame. A zero size pauses drawing (minimised window).
    void resize(uint32_t width_px, uint32_t height_px);

    // Draws one frame and presents it. When `capture` is true the frame is also copied back
    // into readback() before the call returns.
    bool draw_frame(const FrameParams& params, bool capture);

    // Destroys the swapchain (the window's surface is about to go away); set_surface gives the
    // next one to the same device.
    void release_swapchain();
    void set_surface(VkSurfaceKHR surface) {
        surface_ = surface;
        target_dirty_ = true;
    }
    bool has_device() const { return device_ != VK_NULL_HANDLE; }
    void destroy();

    const Readback& readback() const { return readback_; }
    VkExtent2D extent() const { return extent_; }
    uint64_t swapchain_generation() const { return swapchain_generation_; }
    uint64_t frames() const { return frames_; }
    int validation_messages() const { return validation_messages_; }
    const std::string& device_name() const { return device_name_; }
    const std::string& last_error() const { return error_; }
    bool headless() const { return surface_ == VK_NULL_HANDLE; }

#define SPIKE_DECLARE(name) PFN_##name name = nullptr;
    SPIKE_VK_GLOBAL(SPIKE_DECLARE)
    SPIKE_VK_INSTANCE(SPIKE_DECLARE)
    SPIKE_VK_DEVICE(SPIKE_DECLARE)
#undef SPIKE_DECLARE

  private:
    static constexpr uint32_t frames_in_flight = 2;

    struct Frame {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;
        VkFence in_flight = VK_NULL_HANDLE;
    };

    bool fail(std::string message);
    bool create_pipeline();
    bool create_target();
    void destroy_target();
    bool create_readback_buffer(VkDeviceSize size);
    uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags flags) const;
    void record(VkCommandBuffer cmd, VkImage image, VkImageView view, VkImageLayout final_layout,
                const FrameParams& params, bool capture);
    static VKAPI_ATTR VkBool32 VKAPI_CALL on_debug_message(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
        const VkDebugUtilsMessengerCallbackDataEXT* data, void* user);

    void* library_ = nullptr;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;

    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    Frame frame_[frames_in_flight];
    uint32_t frame_index_ = 0;

    // Swapchain, or the offscreen image when headless.
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkSemaphore> render_finished_; // one per swapchain image
    VkImage offscreen_image_ = VK_NULL_HANDLE;
    VkDeviceMemory offscreen_memory_ = VK_NULL_HANDLE;
    bool supports_transfer_src_ = false;

    VkBuffer readback_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory_ = VK_NULL_HANDLE;
    VkDeviceSize readback_size_ = 0;
    Readback readback_;

    VkExtent2D wanted_{0, 0};
    VkExtent2D extent_{0, 0};
    bool target_dirty_ = true;
    uint64_t swapchain_generation_ = 0;
    uint64_t frames_ = 0;
    int validation_messages_ = 0;
    std::string device_name_;
    std::string error_;
};

} // namespace spike
