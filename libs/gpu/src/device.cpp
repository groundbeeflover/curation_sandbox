// SPDX-License-Identifier: Apache-2.0

// The one gpu source that compiles volk and VMA. No VK_USE_PLATFORM_* define is needed: the
// window system's surface extensions are enabled by name, and whoever owns the window (Qt in the
// app) creates the surface with its own function pointers.
#define VOLK_IMPLEMENTATION
#include <volk.h>

// VMA fetches its Vulkan functions through vkGetInstanceProcAddr and vkGetDeviceProcAddr, as
// nothing links the loader. Its implementation is third-party code: its warnings are not ours.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wparentheses"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <vk_mem_alloc.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "cs/gpu/device.hpp"

#include <VkBootstrap.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <utility>

namespace cs::gpu {

namespace {

constexpr const char* validation_layer = "VK_LAYER_KHRONOS_validation";

// CS_GPU_DEVICE comes from the environment: bounded, like any input from outside.
constexpr std::size_t max_device_name_length = 256;

// The window systems' surface extensions; a presentation instance enables those the loader
// offers, so a surface can be created later on whichever the app runs on.
constexpr std::array platform_surface_extensions = {
    "VK_KHR_xcb_surface",   "VK_KHR_xlib_surface",  "VK_KHR_wayland_surface",
    "VK_KHR_win32_surface", "VK_EXT_metal_surface", "VK_KHR_android_surface",
};

std::atomic<bool> device_alive{false};

// Held while a handler runs, so that set_validation_handler waits for it (device.hpp). Recursive,
// so that a handler may replace itself.
std::recursive_mutex handler_mutex;
ValidationHandler validation_handler; // guarded by handler_mutex

ValidationMessage::Severity to_severity(VkDebugUtilsMessageSeverityFlagBitsEXT severity) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        return ValidationMessage::Severity::error;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        return ValidationMessage::Severity::warning;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
        return ValidationMessage::Severity::info;
    return ValidationMessage::Severity::verbose;
}

ValidationMessage::Kind to_kind(VkDebugUtilsMessageTypeFlagsEXT types) {
    if (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)
        return ValidationMessage::Kind::validation;
    if (types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)
        return ValidationMessage::Kind::performance;
    return ValidationMessage::Kind::general;
}

// Called by the layer or the loader inside the Vulkan call that caused the message. Returns
// VK_FALSE, as the specification requires of applications. Nothing may unwind through it.
VKAPI_ATTR VkBool32 VKAPI_CALL on_debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                VkDebugUtilsMessageTypeFlagsEXT types,
                                                const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                void* /*user_data*/) {
    try {
        ValidationMessage message;
        message.severity = to_severity(severity);
        message.kind = to_kind(types);
        if (data != nullptr) {
            message.id = data->pMessageIdName != nullptr ? data->pMessageIdName : "";
            message.text = data->pMessage != nullptr ? data->pMessage : "";
        }
        const std::lock_guard lock(handler_mutex);
        if (validation_handler) {
            // A copy, which stays alive if the handler replaces itself while it runs.
            const ValidationHandler handler = validation_handler;
            handler(message);
        } else {
            std::cerr << to_string(message) << '\n';
        }
    } catch (...) {
        // Out of memory, or the handler threw: print without allocating, and carry on.
        std::fputs(data != nullptr && data->pMessage != nullptr ? data->pMessage
                                                                : "(no message text)",
                   stderr);
        std::fputs("\n(this validation message could not be handled)\n", stderr);
    }
    return VK_FALSE;
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool contains(const std::vector<std::string>& names, std::string_view name) {
    return std::ranges::find(names, name) != names.end();
}

// An environment variable's value, empty when it is not set. MSVC deprecates std::getenv.
std::string environment_variable(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
        return {};
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? value : "";
#endif
}

// The name filter from the options, else from CS_GPU_DEVICE; empty when there is none.
std::string device_name_filter(const DeviceOptions& options) {
    const std::string filter =
        options.device_name ? *options.device_name : environment_variable("CS_GPU_DEVICE");
    if (filter.size() > max_device_name_length)
        throw std::invalid_argument("the device name to choose (CS_GPU_DEVICE) is longer than " +
                                    std::to_string(max_device_name_length) + " characters");
    return filter;
}

// Discrete GPUs first, then integrated, virtual and CPU devices (lavapipe).
int type_rank(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return 0;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return 1;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return 3;
    default:
        return 4;
    }
}

vkb::PhysicalDevice choose_device(const std::vector<vkb::PhysicalDevice>& devices,
                                  const std::string& filter) {
    if (filter.empty()) {
        return *std::ranges::min_element(devices, {}, [](const vkb::PhysicalDevice& d) {
            return type_rank(d.properties.deviceType);
        });
    }
    const std::string wanted = lower(filter);
    for (const vkb::PhysicalDevice& device : devices) {
        if (lower(device.name).find(wanted) != std::string::npos)
            return device;
    }
    std::string names;
    for (const vkb::PhysicalDevice& device : devices)
        names += (names.empty() ? "" : ", ") + device.name;
    throw NoDeviceError("no Vulkan 1.3 device's name contains \"" + filter +
                        "\" (CS_GPU_DEVICE or the device name option); the devices are: " + names);
}

} // namespace

std::string DeviceReport::to_string() const {
    std::ostringstream out;
    out << "device: " << name << '\n'
        << "driver: " << driver << " (" << driver_info << ")\n"
        << "Vulkan: " << VK_API_VERSION_MAJOR(api_version) << '.'
        << VK_API_VERSION_MINOR(api_version) << '.' << VK_API_VERSION_PATCH(api_version) << '\n'
        << "ray query: " << (ray_query ? "yes" : "no") << '\n'
        << "validation: " << (validation ? "on" : "off");
    return out.str();
}

std::string to_string(const ValidationMessage& message) {
    static constexpr std::array kinds = {"general", "validation", "performance"};
    static constexpr std::array severities = {"verbose", "info", "warning", "error"};
    std::string out = std::string(kinds[static_cast<std::size_t>(message.kind)]) + ' ' +
                      severities[static_cast<std::size_t>(message.severity)];
    if (!message.id.empty())
        out += ' ' + message.id;
    return out + ": " + message.text;
}

ValidationHandler set_validation_handler(ValidationHandler handler) {
    const std::lock_guard lock(handler_mutex);
    std::swap(validation_handler, handler);
    return handler;
}

bool gpu_required() {
    const std::string value = environment_variable("CS_REQUIRE_GPU");
    return !value.empty() && value != "0";
}

Device::Device(const DeviceOptions& options) {
    // Checked first, so a mistaken name creates nothing and is not mistaken for "another Device
    // is alive" (std::invalid_argument is a std::logic_error too).
    const std::string name_filter = device_name_filter(options);
    if (device_alive.exchange(true))
        throw std::logic_error("cs::gpu::Device: another Device is alive; volk loads Vulkan "
                               "functions process-wide, so there is one at a time");
    try {
        if (volkInitialize() != VK_SUCCESS)
            throw NoDeviceError("the Vulkan loader (libvulkan) was not found");

        // The validation layer is checked before the instance, so that "not installed" and
        // "disabled by VK_LOADER_LAYERS_DISABLE" are told apart from having no driver.
        auto system = vkb::SystemInfo::get_system_info(vkGetInstanceProcAddr);
        if (!system)
            throw NoDeviceError("the Vulkan loader could not list its layers and extensions: " +
                                system.error().message());
        const vkb::SystemInfo& available = system.value();
        const bool layer_available = available.is_layer_available(validation_layer);
        if (options.validation == Validation::required && !layer_available)
            throw ValidationUnavailableError(
                std::string("the Khronos validation layer (") + validation_layer +
                ") is required but did not load: it is not installed (the Vulkan SDK or the "
                "distribution's validation layers package), or VK_LOADER_LAYERS_DISABLE "
                "disables it");
        const bool validation = options.validation != Validation::off && layer_available;

        // vk-bootstrap's headless mode only means it adds no window-system extensions itself;
        // the surface extensions for presentation are enabled here, by name.
        vkb::InstanceBuilder builder(vkGetInstanceProcAddr);
        builder.set_app_name("Curation Sandbox")
            .set_engine_name("cs")
            .require_api_version(1, 3, 0)
            .set_headless(true);
        const auto enable_instance_extension = [&](const std::string& name) {
            if (!contains(instance_extensions_, name)) {
                builder.enable_extension(name);
                instance_extensions_.push_back(name);
            }
        };
        if (options.presentation) {
            if (!available.is_extension_available(VK_KHR_SURFACE_EXTENSION_NAME))
                throw NoDeviceError("no Vulkan driver offers VK_KHR_surface, which presentation "
                                    "needs");
            enable_instance_extension(VK_KHR_SURFACE_EXTENSION_NAME);
            for (const char* name : platform_surface_extensions) {
                if (available.is_extension_available(name))
                    enable_instance_extension(name);
            }
        }
        for (const std::string& name : options.optional_instance_extensions) {
            if (available.is_extension_available(name.c_str()))
                enable_instance_extension(name);
        }
        if (validation) {
            // Synchronisation validation too: hazards between commands are the custom
            // renderer's main risk (ADR 0001).
            builder.enable_validation_layers(true)
                .add_validation_feature_enable(
                    VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT)
                .set_debug_callback(on_debug_message)
                .set_debug_messenger_severity(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                .set_debug_messenger_type(VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT);
        }
        auto built_instance = builder.build();
        if (!built_instance) {
            const std::string why = built_instance.error().message();
            if (built_instance.matches_error(vkb::InstanceError::requested_layers_not_present) ||
                built_instance.vk_result() == VK_ERROR_LAYER_NOT_PRESENT)
                throw ValidationUnavailableError("the Khronos validation layer did not load: " +
                                                 why);
            throw NoDeviceError("no Vulkan 1.3 instance could be created: " + why);
        }
        const vkb::Instance vkb_instance = built_instance.value();
        instance_ = vkb_instance.instance;
        messenger_ = vkb_instance.debug_messenger;
        volkLoadInstanceOnly(instance_);

        VkPhysicalDeviceVulkan13Features features_13{};
        features_13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features_13.dynamicRendering = VK_TRUE;
        features_13.synchronization2 = VK_TRUE;
        // SV_VertexID in Slang is VertexIndex minus BaseVertex, which needs DrawParameters: the
        // scene's draws give each mesh's first vertex as the base (scene.hpp).
        VkPhysicalDeviceVulkan11Features features_11{};
        features_11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        features_11.shaderDrawParameters = VK_TRUE;
        VkPhysicalDeviceVulkan12Features features_12{};
        features_12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        features_12.bufferDeviceAddress = VK_TRUE;
        vkb::PhysicalDeviceSelector selector(vkb_instance);
        selector.set_minimum_version(1, 3)
            .set_required_features_13(features_13)
            .set_required_features_12(features_12)
            .set_required_features_11(features_11)
            .allow_any_gpu_device_type(true);
        if (options.presentation)
            selector.add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        auto devices = selector.select_devices();
        if (!devices || devices.value().empty())
            throw NoDeviceError(
                "no Vulkan device with Vulkan 1.3, dynamic rendering, synchronization2, "
                "buffer device address and shader draw parameters" +
                std::string(options.presentation ? " and VK_KHR_swapchain" : "") + ": " +
                (devices ? std::string("none found") : devices.error().message()));
        const vkb::PhysicalDevice chosen = choose_device(devices.value(), name_filter);
        physical_device_ = chosen.physical_device;

        auto built_device = vkb::DeviceBuilder(chosen).build();
        if (!built_device)
            throw std::runtime_error("cs::gpu::Device: creating the device on " + chosen.name +
                                     " failed: " + built_device.error().message());
        const vkb::Device vkb_device = built_device.value();
        device_ = vkb_device.device;
        volkLoadDevice(device_);
        const auto queue = vkb_device.get_queue_and_index(vkb::QueueType::graphics);
        if (!queue)
            throw std::runtime_error("cs::gpu::Device: " + chosen.name +
                                     " has no graphics queue: " + queue.error().message());
        std::tie(queue_, queue_family_) = queue.value();
        const VkQueueFlags flags = chosen.get_queue_families()[queue_family_].queueFlags;
        if ((flags & VK_QUEUE_COMPUTE_BIT) == 0)
            throw std::runtime_error("cs::gpu::Device: the graphics queue of " + chosen.name +
                                     " cannot run compute work");
        extensions_ = chosen.get_extensions();

        VmaVulkanFunctions functions{};
        functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
        functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
        VmaAllocatorCreateInfo allocator_info{};
        allocator_info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        allocator_info.vulkanApiVersion = VK_API_VERSION_1_3;
        allocator_info.physicalDevice = physical_device_;
        allocator_info.device = device_;
        allocator_info.instance = instance_;
        allocator_info.pVulkanFunctions = &functions;
        if (const VkResult result = vmaCreateAllocator(&allocator_info, &allocator_);
            result != VK_SUCCESS)
            throw std::runtime_error("cs::gpu::Device: creating the VMA allocator failed (" +
                                     std::to_string(static_cast<int>(result)) + ")");

        VkPhysicalDeviceVulkan12Properties properties_12{};
        properties_12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
        VkPhysicalDeviceProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &properties_12;
        vkGetPhysicalDeviceProperties2(physical_device_, &properties);
        report_.name = properties.properties.deviceName;
        report_.driver = properties_12.driverName;
        report_.driver_info = properties_12.driverInfo;
        report_.api_version = properties.properties.apiVersion;
        report_.ray_query =
            contains(chosen.get_available_extensions(), VK_KHR_RAY_QUERY_EXTENSION_NAME);
        report_.validation = validation;
    } catch (...) {
        destroy();
        throw;
    }
}

Device::~Device() {
    destroy();
}

void Device::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (allocator_ != VK_NULL_HANDLE)
            vmaDestroyAllocator(allocator_);
        vkDestroyDevice(device_, nullptr);
    }
    if (messenger_ != VK_NULL_HANDLE)
        vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    if (instance_ != VK_NULL_HANDLE)
        vkDestroyInstance(instance_, nullptr);
    allocator_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    messenger_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
    device_alive = false;
}

bool Device::has_extension(std::string_view name) const {
    return contains(extensions_, name);
}

bool Device::has_instance_extension(std::string_view name) const {
    return contains(instance_extensions_, name);
}

bool Device::can_present(VkSurfaceKHR surface) const {
    if (!presentation() || surface == VK_NULL_HANDLE)
        return false;
    VkBool32 supported = VK_FALSE;
    return vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, queue_family_, surface,
                                                &supported) == VK_SUCCESS &&
           supported == VK_TRUE;
}

VkResult Device::wait_idle() const {
    return vkDeviceWaitIdle(device_);
}

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
