// SPDX-License-Identifier: Apache-2.0
#include "vk_renderer.hpp"

#include "triangle_spv.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace spike {

namespace {

void* open_loader() {
#if defined(_WIN32)
    return reinterpret_cast<void*>(LoadLibraryA("vulkan-1.dll"));
#elif defined(__APPLE__)
    void* lib = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);
    return lib ? lib : dlopen("libMoltenVK.dylib", RTLD_NOW | RTLD_LOCAL);
#else
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    return lib ? lib : dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
#endif
}

void* find_symbol(void* lib, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

void close_loader(void* lib) {
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(lib));
#else
    dlclose(lib);
#endif
}

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    return std::any_of(list.begin(), list.end(), [&](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}

VkImageMemoryBarrier2 image_barrier(VkImage image, VkImageLayout from, VkImageLayout to,
                                    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}

} // namespace

VkRenderer::~VkRenderer() {
    destroy();
}

bool VkRenderer::fail(std::string message) {
    error_ = std::move(message);
    std::fprintf(stderr, "[renderer] %s\n", error_.c_str());
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL VkRenderer::on_debug_message(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    auto* self = static_cast<VkRenderer*>(user);
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT &&
        (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)) {
        ++self->validation_messages_;
    }
    std::fprintf(stderr, "[validation] %s\n", data->pMessage);
    return VK_FALSE;
}

bool VkRenderer::create_instance(bool validation) {
    library_ = open_loader();
    if (!library_) {
        return fail("the Vulkan loader was not found");
    }
    get_instance_proc_addr_ =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(find_symbol(library_, "vkGetInstanceProcAddr"));
    if (!get_instance_proc_addr_) {
        return fail("vkGetInstanceProcAddr is missing from the loader");
    }
#define SPIKE_LOAD_GLOBAL(name)                                                                    \
    name = reinterpret_cast<PFN_##name>(get_instance_proc_addr_(VK_NULL_HANDLE, #name));
    SPIKE_VK_GLOBAL(SPIKE_LOAD_GLOBAL)
#undef SPIKE_LOAD_GLOBAL

    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());

    std::vector<const char*> extensions;
    VkInstanceCreateFlags flags = 0;
    for (const char* name :
         {"VK_KHR_surface", "VK_KHR_xcb_surface", "VK_KHR_xlib_surface", "VK_KHR_wayland_surface",
          "VK_KHR_win32_surface", "VK_EXT_metal_surface", "VK_KHR_portability_enumeration"}) {
        if (has_extension(available, name)) {
            extensions.push_back(name);
            if (std::strcmp(name, "VK_KHR_portability_enumeration") == 0) {
                flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            }
        }
    }

    std::vector<const char*> layers;
    if (validation) {
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> layer_list(count);
        vkEnumerateInstanceLayerProperties(&count, layer_list.data());
        const bool has_layer =
            std::any_of(layer_list.begin(), layer_list.end(), [](const VkLayerProperties& l) {
                return std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0;
            });
        if (has_layer && has_extension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else {
            std::fprintf(stderr, "[renderer] validation layer not installed; running without\n");
        }
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "qt-vulkan-spike";
    app.apiVersion = VK_API_VERSION_1_3;

    VkDebugUtilsMessengerCreateInfoEXT messenger_info{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messenger_info.pfnUserCallback = &VkRenderer::on_debug_message;
    messenger_info.pUserData = this;

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pNext = layers.empty() ? nullptr : &messenger_info;
    info.flags = flags;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    if (vkCreateInstance(&info, nullptr, &instance_) != VK_SUCCESS) {
        return fail("vkCreateInstance failed");
    }

#define SPIKE_LOAD_INSTANCE(name)                                                                  \
    name = reinterpret_cast<PFN_##name>(get_instance_proc_addr_(instance_, #name));
    SPIKE_VK_INSTANCE(SPIKE_LOAD_INSTANCE)
#undef SPIKE_LOAD_INSTANCE

    if (!layers.empty() && vkCreateDebugUtilsMessengerEXT) {
        vkCreateDebugUtilsMessengerEXT(instance_, &messenger_info, nullptr, &messenger_);
    }
    return true;
}

bool VkRenderer::create_device(VkSurfaceKHR surface) {
    surface_ = surface;
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    int best_score = -1;
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) {
            continue;
        }
        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> families(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, families.data());
        for (uint32_t q = 0; q < qcount; ++q) {
            if (!(families[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                continue;
            }
            VkBool32 present = VK_TRUE;
            if (surface_) {
                vkGetPhysicalDeviceSurfaceSupportKHR(pd, q, surface_, &present);
            }
            if (!present) {
                continue;
            }
            const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                              : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                                                                                           : 1;
            if (score > best_score) {
                best_score = score;
                physical_ = pd;
                queue_family_ = q;
                device_name_ = props.deviceName;
            }
            break;
        }
    }
    if (!physical_) {
        return fail("no Vulkan 1.3 device can draw (and present to this surface)");
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = queue_family_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    VkPhysicalDeviceVulkan13Features features13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    const char* swapchain_ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &features13;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue_info;
    info.enabledExtensionCount = surface_ ? 1 : 0;
    info.ppEnabledExtensionNames = surface_ ? &swapchain_ext : nullptr;
    if (vkCreateDevice(physical_, &info, nullptr, &device_) != VK_SUCCESS) {
        return fail("vkCreateDevice failed");
    }
#define SPIKE_LOAD_DEVICE(name)                                                                    \
    name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device_, #name));
    SPIKE_VK_DEVICE(SPIKE_LOAD_DEVICE)
#undef SPIKE_LOAD_DEVICE
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    if (surface_) {
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, formats.data());
        format_ = formats.empty() ? VK_FORMAT_UNDEFINED : formats[0].format;
        for (const VkSurfaceFormatKHR& f : formats) {
            if ((f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                format_ = f.format;
                break;
            }
        }
    } else {
        format_ = VK_FORMAT_R8G8B8A8_SRGB;
    }

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family_;
    vkCreateCommandPool(device_, &pool_info, nullptr, &pool_);
    for (Frame& f : frame_) {
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = pool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        vkAllocateCommandBuffers(device_, &alloc, &f.cmd);
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device_, &sem, nullptr, &f.image_available);
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(device_, &fence, nullptr, &f.in_flight);
    }
    return create_pipeline();
}

bool VkRenderer::create_pipeline() {
    VkShaderModule modules[2] = {};
    const uint32_t* code[2] = {triangle_vert_spv, triangle_frag_spv};
    const size_t size[2] = {sizeof(triangle_vert_spv), sizeof(triangle_frag_spv)};
    for (int i = 0; i < 2; ++i) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = size[i];
        info.pCode = code[i];
        vkCreateShaderModule(device_, &info, nullptr, &modules[i]);
    }

    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, 4 * sizeof(float)};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &range;
    vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_);

    VkPipelineShaderStageCreateInfo stages[2] = {};
    for (int i = 0; i < 2; ++i) {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[i].module = modules[i];
        stages[i].pName = "main";
    }
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format_;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = pipeline_layout_;
    const VkResult result =
        vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
    for (VkShaderModule m : modules) {
        vkDestroyShaderModule(device_, m, nullptr);
    }
    return result == VK_SUCCESS || fail("vkCreateGraphicsPipelines failed");
}

uint32_t VkRenderer::find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(physical_, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    return UINT32_MAX;
}

void VkRenderer::resize(uint32_t width_px, uint32_t height_px) {
    if (width_px != wanted_.width || height_px != wanted_.height) {
        wanted_ = {width_px, height_px};
        target_dirty_ = true;
    }
}

void VkRenderer::destroy_target() {
    for (VkImageView v : views_) {
        vkDestroyImageView(device_, v, nullptr);
    }
    views_.clear();
    for (VkSemaphore s : render_finished_) {
        vkDestroySemaphore(device_, s, nullptr);
    }
    render_finished_.clear();
    images_.clear();
    if (offscreen_image_) {
        vkDestroyImage(device_, offscreen_image_, nullptr);
        vkFreeMemory(device_, offscreen_memory_, nullptr);
        offscreen_image_ = VK_NULL_HANDLE;
        offscreen_memory_ = VK_NULL_HANDLE;
    }
}

bool VkRenderer::create_target() {
    vkDeviceWaitIdle(device_);
    destroy_target();
    extent_ = wanted_;

    if (surface_) {
        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps);
        if (caps.currentExtent.width != UINT32_MAX) {
            extent_ = caps.currentExtent; // X11 and Windows: the window decides
        } else {
            extent_.width =
                std::clamp(extent_.width, caps.minImageExtent.width, caps.maxImageExtent.width);
            extent_.height =
                std::clamp(extent_.height, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (extent_.width == 0 || extent_.height == 0) {
            return true; // minimised; try again at the next resize
        }
        uint32_t image_count = caps.minImageCount + 1;
        if (caps.maxImageCount > 0) {
            image_count = std::min(image_count, caps.maxImageCount);
        }
        supports_transfer_src_ = caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if (!(caps.supportedCompositeAlpha & alpha)) {
            alpha = static_cast<VkCompositeAlphaFlagBitsKHR>(caps.supportedCompositeAlpha &
                                                             -caps.supportedCompositeAlpha);
        }

        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface_;
        info.minImageCount = image_count;
        info.imageFormat = format_;
        info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        info.imageExtent = extent_;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                          (supports_transfer_src_ ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = alpha;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        info.oldSwapchain = swapchain_;
        VkSwapchainKHR created = VK_NULL_HANDLE;
        const VkResult result = vkCreateSwapchainKHR(device_, &info, nullptr, &created);
        if (swapchain_) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        }
        swapchain_ = created;
        if (result != VK_SUCCESS) {
            return fail("vkCreateSwapchainKHR failed");
        }
        uint32_t count = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
        images_.resize(count);
        vkGetSwapchainImagesKHR(device_, swapchain_, &count, images_.data());
    } else {
        if (extent_.width == 0 || extent_.height == 0) {
            return true;
        }
        supports_transfer_src_ = true;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format_;
        info.extent = {extent_.width, extent_.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        vkCreateImage(device_, &info, nullptr, &offscreen_image_);
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, offscreen_image_, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex =
            find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(device_, &alloc, nullptr, &offscreen_memory_);
        vkBindImageMemory(device_, offscreen_image_, offscreen_memory_, 0);
        images_.push_back(offscreen_image_);
    }

    for (VkImage image : images_) {
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image = image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = format_;
        info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(device_, &info, nullptr, &view);
        views_.push_back(view);
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s = VK_NULL_HANDLE;
        vkCreateSemaphore(device_, &sem, nullptr, &s);
        render_finished_.push_back(s);
    }
    ++swapchain_generation_;
    std::fprintf(stderr, "[renderer] %s target %ux%u (generation %llu)\n",
                 surface_ ? "swapchain" : "offscreen", extent_.width, extent_.height,
                 static_cast<unsigned long long>(swapchain_generation_));
    return create_readback_buffer(VkDeviceSize(extent_.width) * extent_.height * 4);
}

bool VkRenderer::create_readback_buffer(VkDeviceSize size) {
    if (readback_buffer_ && readback_size_ >= size) {
        return true;
    }
    if (readback_buffer_) {
        vkDestroyBuffer(device_, readback_buffer_, nullptr);
        vkFreeMemory(device_, readback_memory_, nullptr);
    }
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkCreateBuffer(device_, &info, nullptr, &readback_buffer_);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, readback_buffer_, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex =
        find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(device_, &alloc, nullptr, &readback_memory_) != VK_SUCCESS) {
        return fail("no host-visible memory for the readback buffer");
    }
    vkBindBufferMemory(device_, readback_buffer_, readback_memory_, 0);
    readback_size_ = size;
    return true;
}

void VkRenderer::record(VkCommandBuffer cmd, VkImage image, VkImageView view,
                        VkImageLayout final_layout, const FrameParams& params, bool capture) {
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    VkImageMemoryBarrier2 to_attachment = image_barrier(
        image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &to_attachment;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkRenderingAttachmentInfo colour{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colour.imageView = view;
    colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colour.clearValue.color = {{0.02f, 0.025f, 0.03f, 1.0f}};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, extent_};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &colour;
    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const VkViewport vp{0, 0, float(extent_.width), float(extent_.height), 0, 1};
    const VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    const float push[4] = {params.angle_rad, float(extent_.width) / float(extent_.height),
                           params.offset_y, 0.0f};
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkAccessFlags2 access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    if (capture) {
        VkImageMemoryBarrier2 to_transfer =
            image_barrier(image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stage, access,
                          VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        dep.pImageMemoryBarriers = &to_transfer;
        vkCmdPipelineBarrier2(cmd, &dep);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent_.width, extent_.height, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buffer_,
                               1, &region);
        VkBufferMemoryBarrier2 to_host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        to_host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_host.buffer = readback_buffer_;
        to_host.size = VK_WHOLE_SIZE;
        VkDependencyInfo host_dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        host_dep.bufferMemoryBarrierCount = 1;
        host_dep.pBufferMemoryBarriers = &to_host;
        vkCmdPipelineBarrier2(cmd, &host_dep);
        layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        stage = VK_PIPELINE_STAGE_2_COPY_BIT;
        access = 0;
    }
    if (layout != final_layout) {
        VkImageMemoryBarrier2 to_final =
            image_barrier(image, layout, final_layout, stage, access, VK_PIPELINE_STAGE_2_NONE, 0);
        dep.pImageMemoryBarriers = &to_final;
        vkCmdPipelineBarrier2(cmd, &dep);
    }
    vkEndCommandBuffer(cmd);
}

bool VkRenderer::draw_frame(const FrameParams& params, bool capture) {
    if (!device_ || wanted_.width == 0 || wanted_.height == 0) {
        return true;
    }
    if (target_dirty_ || (surface_ && !swapchain_)) {
        target_dirty_ = false;
        if (!create_target()) {
            return false;
        }
    }
    if (images_.empty()) {
        return true; // minimised
    }
    capture = capture && supports_transfer_src_;

    Frame& f = frame_[frame_index_];
    vkWaitForFences(device_, 1, &f.in_flight, VK_TRUE, UINT64_MAX);

    uint32_t image_index = 0;
    if (swapchain_) {
        const VkResult acquired = vkAcquireNextImageKHR(
            device_, swapchain_, UINT64_MAX, f.image_available, VK_NULL_HANDLE, &image_index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            target_dirty_ = true;
            return true;
        }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
            return fail("vkAcquireNextImageKHR failed");
        }
    }
    vkResetFences(device_, 1, &f.in_flight);
    vkResetCommandBuffer(f.cmd, 0);
    record(f.cmd, images_[image_index], views_[image_index],
           swapchain_ ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
           params, capture);

    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = f.image_available;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = render_finished_[image_index];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd_info.commandBuffer = f.cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = swapchain_ ? 1 : 0;
    submit.pWaitSemaphoreInfos = &wait;
    submit.signalSemaphoreInfoCount = swapchain_ ? 1 : 0;
    submit.pSignalSemaphoreInfos = &signal;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    if (vkQueueSubmit2(queue_, 1, &submit, f.in_flight) != VK_SUCCESS) {
        return fail("vkQueueSubmit2 failed");
    }

    if (swapchain_) {
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &render_finished_[image_index];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &image_index;
        const VkResult presented = vkQueuePresentKHR(queue_, &present);
        if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
            target_dirty_ = true;
        } else if (presented != VK_SUCCESS) {
            return fail("vkQueuePresentKHR failed");
        }
    }

    if (capture) {
        vkWaitForFences(device_, 1, &f.in_flight, VK_TRUE, UINT64_MAX);
        void* mapped = nullptr;
        vkMapMemory(device_, readback_memory_, 0, VK_WHOLE_SIZE, 0, &mapped);
        readback_.width = extent_.width;
        readback_.height = extent_.height;
        readback_.rgba.resize(size_t(extent_.width) * extent_.height * 4);
        std::memcpy(readback_.rgba.data(), mapped, readback_.rgba.size());
        vkUnmapMemory(device_, readback_memory_);
        if (format_ == VK_FORMAT_B8G8R8A8_SRGB || format_ == VK_FORMAT_B8G8R8A8_UNORM) {
            for (size_t i = 0; i < readback_.rgba.size(); i += 4) {
                std::swap(readback_.rgba[i], readback_.rgba[i + 2]);
            }
        }
    }
    frame_index_ = (frame_index_ + 1) % frames_in_flight;
    ++frames_;
    return true;
}

void VkRenderer::release_swapchain() {
    if (!device_) {
        return;
    }
    vkDeviceWaitIdle(device_);
    destroy_target();
    if (swapchain_) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    target_dirty_ = true;
}

void VkRenderer::destroy() {
    if (device_) {
        release_swapchain();
        if (readback_buffer_) {
            vkDestroyBuffer(device_, readback_buffer_, nullptr);
            vkFreeMemory(device_, readback_memory_, nullptr);
            readback_buffer_ = VK_NULL_HANDLE;
        }
        for (Frame& f : frame_) {
            vkDestroySemaphore(device_, f.image_available, nullptr);
            vkDestroyFence(device_, f.in_flight, nullptr);
            f = Frame{};
        }
        vkDestroyCommandPool(device_, pool_, nullptr);
        vkDestroyPipeline(device_, pipeline_, nullptr);
        vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_) {
        if (messenger_) {
            vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
            messenger_ = VK_NULL_HANDLE;
        }
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    if (library_) {
        close_loader(library_);
        library_ = nullptr;
    }
}

} // namespace spike
