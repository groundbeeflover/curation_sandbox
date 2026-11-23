// SPDX-License-Identifier: Apache-2.0

// The Vulkan 1.3 device (device.hpp, issue M1-19): headless or ready to present, chosen by name,
// with a report for logs and CI, and every validation message routed to a handler.
//
// Every test that needs a device starts with make_device() (gpu_test.hpp): with no Vulkan device
// each one skips, and with CS_REQUIRE_GPU=1 each one fails saying no device was found (ADR 0010).
// In a debug build with CS_REQUIRE_GPU=1 the validation layer is required, so a layer that fails
// to load fails the test with ValidationUnavailableError's message instead of running unchecked
// (ADR 0011). The harness (main.cpp) fails the running test on any validation warning or error,
// so these tests also check that the device itself is validation-clean.

#include "gpu_test.hpp"

#include <cs/gpu/device.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::MessageMatches;
using cs::gpu::Device;
using cs::gpu::DeviceOptions;
using cs::gpu::NoDeviceError;
using cs::gpu::Validation;
using cs::gpu::ValidationMessage;
using cs::gpu::ValidationUnavailableError;
using cs::gpu::test::Cleanup;
using cs::gpu::test::HandlerGuard;
using cs::gpu::test::make_device;
using cs::gpu::test::ScopedEnv;
using cs::gpu::test::test_options;
using cs::gpu::test::validation_layer_required;

namespace {

// A name no real device has, for the "no matching device" cases.
constexpr const char* no_such_device = "no such device 7f3a";

std::string to_upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char c) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    });
    return text;
}

std::string version_string(std::uint32_t version) {
    return std::to_string(VK_API_VERSION_MAJOR(version)) + "." +
           std::to_string(VK_API_VERSION_MINOR(version)) + "." +
           std::to_string(VK_API_VERSION_PATCH(version));
}

// Whether the loader lists the Khronos validation layer. One that honours VK_LOADER_LAYERS_DISABLE
// leaves a layer it names out of the list, as it leaves it out of every instance. Needs volk's
// loader functions: any Device made earlier in the process has loaded them.
bool loader_lists_validation_layer() {
    REQUIRE(vkEnumerateInstanceLayerProperties != nullptr);
    std::uint32_t count = 0;
    REQUIRE(vkEnumerateInstanceLayerProperties(&count, nullptr) == VK_SUCCESS);
    std::vector<VkLayerProperties> layers(count);
    REQUIRE(vkEnumerateInstanceLayerProperties(&count, layers.data()) == VK_SUCCESS);
    layers.resize(count);
    return std::any_of(layers.begin(), layers.end(), [](const VkLayerProperties& layer) {
        return std::string(layer.layerName) == "VK_LAYER_KHRONOS_validation";
    });
}

// With VK_LOADER_LAYERS_DISABLE naming the validation layer, when the loader still lists the
// layer, because it ignores the variable: on Windows, where an elevated process's loader does so
// (CI's runner, ADR 0010), the test skips, saying why. Elsewhere no loader the tests support does
// (1.3.234 or later), so the test fails, saying why, rather than leave ADR 0011 untested.
void skip_if_loader_ignores_layers_disable() {
    if (!loader_lists_validation_layer()) {
        return;
    }
#ifdef _WIN32
    SKIP("the Vulkan loader ignores VK_LOADER_LAYERS_DISABLE here, as it does in an elevated "
         "process on Windows (ADR 0010), so the validation layer cannot be disabled");
#else
    FAIL("the Vulkan loader ignores VK_LOADER_LAYERS_DISABLE, so the validation layer cannot be "
         "disabled: the variable needs loader 1.3.234 or later");
#endif
}

} // namespace

// ---- Criterion 1: Vulkan 1.3 with dynamic rendering, synchronization2, buffer device address and
// VMA, headless without VK_KHR_swapchain
// ------------------------------------------------------------

TEST_CASE("device: a headless device renders with dynamic rendering, synchronization2, buffer "
          "device address and VMA",
          "[gpu]") {
    const std::unique_ptr<Device> gpu = make_device();

    REQUIRE(gpu->instance() != VK_NULL_HANDLE);
    REQUIRE(gpu->physical_device() != VK_NULL_HANDLE);
    REQUIRE(gpu->device() != VK_NULL_HANDLE);
    REQUIRE(gpu->queue() != VK_NULL_HANDLE);
    REQUIRE(gpu->allocator() != VK_NULL_HANDLE);
    CHECK(gpu->report().api_version >= VK_API_VERSION_1_3);
    CHECK_FALSE(gpu->presentation());
    CHECK_FALSE(gpu->has_extension("VK_KHR_swapchain"));

    const VkDevice device = gpu->device();
    const VmaAllocator allocator = gpu->allocator();
    constexpr std::uint32_t width = 4;
    constexpr std::uint32_t height = 4;
    constexpr VkDeviceSize texel_bytes = 4; // R8G8B8A8
    constexpr VkDeviceSize buffer_bytes = width * height * texel_bytes;

    // The colour image the clear renders into, from VMA.
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo image_allocation_info{};
    image_allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation image_allocation = VK_NULL_HANDLE;
    REQUIRE(vmaCreateImage(allocator, &image_info, &image_allocation_info, &image,
                           &image_allocation, nullptr) == VK_SUCCESS);
    const Cleanup destroy_image{[&] { vmaDestroyImage(allocator, image, image_allocation); }};

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    REQUIRE(vkCreateImageView(device, &view_info, nullptr, &view) == VK_SUCCESS);
    const Cleanup destroy_view{[&] { vkDestroyImageView(device, view, nullptr); }};

    // The host-visible readback buffer, also with a device address, from VMA.
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = buffer_bytes;
    buffer_info.usage =
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo buffer_allocation_info{};
    buffer_allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
    buffer_allocation_info.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation buffer_allocation = VK_NULL_HANDLE;
    VmaAllocationInfo buffer_allocation_result{};
    REQUIRE(vmaCreateBuffer(allocator, &buffer_info, &buffer_allocation_info, &buffer,
                            &buffer_allocation, &buffer_allocation_result) == VK_SUCCESS);
    const Cleanup destroy_buffer{[&] { vmaDestroyBuffer(allocator, buffer, buffer_allocation); }};
    REQUIRE(buffer_allocation_result.pMappedData != nullptr);
    auto* const texels = static_cast<std::uint8_t*>(buffer_allocation_result.pMappedData);

    SECTION("buffer device address: a buffer has a non-zero address") {
        VkBufferDeviceAddressInfo address_info{};
        address_info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        address_info.buffer = buffer;
        CHECK(vkGetBufferDeviceAddress(device, &address_info) != 0);
    }

    SECTION("dynamic rendering and synchronization2: a cleared image reads back as the clear "
            "colour") {
        // Fill the buffer with a pattern the clear colour does not contain, so a copy that never
        // happened cannot pass.
        std::memset(texels, 0xAB, static_cast<std::size_t>(buffer_bytes));
        REQUIRE(vmaFlushAllocation(allocator, buffer_allocation, 0, VK_WHOLE_SIZE) == VK_SUCCESS);

        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex = gpu->queue_family();
        VkCommandPool pool = VK_NULL_HANDLE;
        REQUIRE(vkCreateCommandPool(device, &pool_info, nullptr, &pool) == VK_SUCCESS);
        const Cleanup destroy_pool{[&] { vkDestroyCommandPool(device, pool, nullptr); }};

        VkCommandBufferAllocateInfo command_info{};
        command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_info.commandPool = pool;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        REQUIRE(vkAllocateCommandBuffers(device, &command_info, &commands) == VK_SUCCESS);

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        REQUIRE(vkCreateFence(device, &fence_info, nullptr, &fence) == VK_SUCCESS);
        // Declared after the pool, so destroyed first; the wait below (or wait_idle on failure)
        // guarantees the GPU is done with both.
        const Cleanup destroy_fence{[&] {
            vkDeviceWaitIdle(device); // a destructor cannot act on a lost device anyway
            vkDestroyFence(device, fence, nullptr);
        }};

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        REQUIRE(vkBeginCommandBuffer(commands, &begin_info) == VK_SUCCESS);

        const VkImageSubresourceRange colour_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // Undefined -> colour attachment, before the clear writes it. No source stage: the image
        // is new. An attachment written before would need COLOR_ATTACHMENT_OUTPUT as source.
        VkImageMemoryBarrier2 to_attachment{};
        to_attachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_attachment.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        to_attachment.srcAccessMask = VK_ACCESS_2_NONE;
        to_attachment.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_attachment.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_attachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_attachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_attachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_attachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_attachment.image = image;
        to_attachment.subresourceRange = colour_range;
        VkDependencyInfo to_attachment_dependency{};
        to_attachment_dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        to_attachment_dependency.imageMemoryBarrierCount = 1;
        to_attachment_dependency.pImageMemoryBarriers = &to_attachment;
        vkCmdPipelineBarrier2(commands, &to_attachment_dependency);

        // The clear colour. 1 and 0 convert to UNORM8 exactly (255 and 0). 0.2 and 0.6 are 51 and
        // 153 times 1/255, but 0.2f and 0.6f are not exact in binary, and the Vulkan
        // specification ("Conversion from Floating-Point to Normalized Fixed-Point") lets an
        // implementation return either of the two integers nearest to f * 255. So those two
        // channels are checked to within 1 (one UNORM8 step), the others exactly.
        VkClearValue clear{};
        clear.color.float32[0] = 1.0f;
        clear.color.float32[1] = 0.2f;
        clear.color.float32[2] = 0.0f;
        clear.color.float32[3] = 0.6f;
        constexpr std::array<int, 4> expected{255, 51, 0, 153};
        constexpr std::array<int, 4> tolerance{0, 1, 0, 1};

        VkRenderingAttachmentInfo attachment{};
        attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        attachment.imageView = view;
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.clearValue = clear;
        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = {{0, 0}, {width, height}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
        vkCmdBeginRendering(commands, &rendering);
        vkCmdEndRendering(commands);

        // Colour attachment -> transfer source, after the clear's store.
        VkImageMemoryBarrier2 to_transfer{};
        to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_transfer.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_transfer.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_transfer.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_transfer.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        to_transfer.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.image = image;
        to_transfer.subresourceRange = colour_range;
        VkDependencyInfo to_transfer_dependency{};
        to_transfer_dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        to_transfer_dependency.imageMemoryBarrierCount = 1;
        to_transfer_dependency.pImageMemoryBarriers = &to_transfer;
        vkCmdPipelineBarrier2(commands, &to_transfer_dependency);

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0; // tightly packed
        region.bufferImageHeight = 0;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
                               &region);

        // Transfer write -> host read, so the mapped memory sees the copy after the fence.
        VkBufferMemoryBarrier2 to_host{};
        to_host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        to_host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_host.buffer = buffer;
        to_host.offset = 0;
        to_host.size = VK_WHOLE_SIZE;
        VkDependencyInfo to_host_dependency{};
        to_host_dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        to_host_dependency.bufferMemoryBarrierCount = 1;
        to_host_dependency.pBufferMemoryBarriers = &to_host;
        vkCmdPipelineBarrier2(commands, &to_host_dependency);

        REQUIRE(vkEndCommandBuffer(commands) == VK_SUCCESS);

        VkCommandBufferSubmitInfo command_submit{};
        command_submit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        command_submit.commandBuffer = commands;
        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &command_submit;
        REQUIRE(vkQueueSubmit2(gpu->queue(), 1, &submit, fence) == VK_SUCCESS);
        // Ten seconds: a 4x4 clear takes microseconds even on lavapipe; the bound only keeps a
        // broken driver from hanging the test run.
        constexpr std::uint64_t timeout_ns = 10'000'000'000;
        REQUIRE(vkWaitForFences(device, 1, &fence, VK_TRUE, timeout_ns) == VK_SUCCESS);
        REQUIRE(vmaInvalidateAllocation(allocator, buffer_allocation, 0, VK_WHOLE_SIZE) ==
                VK_SUCCESS);

        for (std::uint32_t texel = 0; texel < width * height; ++texel) {
            for (std::size_t channel = 0; channel < 4; ++channel) {
                const int value = texels[texel * texel_bytes + channel];
                CAPTURE(texel, channel, value, expected[channel]);
                CHECK(std::abs(value - expected[channel]) <= tolerance[channel]);
            }
        }
    }
}

// ---- Criterion 1: works with a surface supplied later, enabling VK_KHR_swapchain ----------------

TEST_CASE("device: a device for presentation enables VK_KHR_swapchain and presents to a surface "
          "created later",
          "[gpu]") {
    DeviceOptions options = test_options();
    options.presentation = true;
    // A surface without a window, so that the test runs on CI; the loader offers it on Linux and
    // Windows when the driver supports it, and the device skips it silently when not.
    options.optional_instance_extensions = {"VK_EXT_headless_surface"};
    const std::unique_ptr<Device> gpu = make_device(options);

    CHECK(gpu->presentation());
    CHECK(gpu->has_extension("VK_KHR_swapchain"));
    CHECK(gpu->has_instance_extension("VK_KHR_surface"));

    if (!gpu->has_instance_extension("VK_EXT_headless_surface")) {
        WARN("VK_EXT_headless_surface is not offered, so can_present() on a surface created "
             "later is not checked");
        return;
    }

    REQUIRE(vkCreateHeadlessSurfaceEXT != nullptr);
    REQUIRE(vkDestroySurfaceKHR != nullptr);
    VkHeadlessSurfaceCreateInfoEXT surface_info{};
    surface_info.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    REQUIRE(vkCreateHeadlessSurfaceEXT(gpu->instance(), &surface_info, nullptr, &surface) ==
            VK_SUCCESS);
    const VkInstance instance = gpu->instance();
    const Cleanup destroy_surface{[&] { vkDestroySurfaceKHR(instance, surface, nullptr); }};

    CHECK(gpu->can_present(surface));
}

// Header (DeviceOptions::optional_instance_extensions): enabled when offered, skipped silently
// otherwise.
TEST_CASE("device: an optional instance extension the loader does not offer is skipped", "[gpu]") {
    DeviceOptions options = test_options();
    options.optional_instance_extensions = {"VK_CS_no_such_extension_7f3a"};
    const std::unique_ptr<Device> gpu = make_device(options);

    CHECK_FALSE(gpu->has_instance_extension("VK_CS_no_such_extension_7f3a"));
    CHECK(gpu->device() != VK_NULL_HANDLE);
}

// ---- Criterion 2: CS_GPU_DEVICE (a substring of the device name) chooses the device -------------

TEST_CASE("device: device_name and CS_GPU_DEVICE choose the device by a substring of its name, "
          "ignoring case",
          "[gpu]") {
    std::string name;
    {
        const std::unique_ptr<Device> probe = make_device();
        name = probe->report().name;
    }
    REQUIRE(name.size() >= 2);
    // A substring that is not a prefix, in another case: everything after the first character,
    // upper-cased. Long, so that it matches no other device on a machine with several.
    const std::string pattern = to_upper(name.substr(1));
    CAPTURE(name, pattern);

    SECTION("device_name chooses the device") {
        DeviceOptions options = test_options();
        options.device_name = pattern;
        const Device gpu{options};
        CHECK(gpu.report().name == name);
    }

    SECTION("a device_name that matches nothing throws NoDeviceError naming the devices") {
        DeviceOptions options = test_options();
        options.device_name = no_such_device;
        CHECK_THROWS_MATCHES(Device{options}, NoDeviceError,
                             MessageMatches(ContainsSubstring(name)));
    }

    SECTION("CS_GPU_DEVICE chooses the device when device_name is unset") {
        const ScopedEnv env{"CS_GPU_DEVICE", pattern};
        DeviceOptions options = test_options();
        options.device_name.reset();
        const Device gpu{options};
        CHECK(gpu.report().name == name);
    }

    SECTION("a CS_GPU_DEVICE that matches nothing throws NoDeviceError") {
        const ScopedEnv env{"CS_GPU_DEVICE", std::string(no_such_device)};
        DeviceOptions options = test_options();
        options.device_name.reset();
        CHECK_THROWS_AS(Device{options}, NoDeviceError);
    }

    SECTION("device_name wins over CS_GPU_DEVICE") {
        const ScopedEnv env{"CS_GPU_DEVICE", std::string(no_such_device)};
        DeviceOptions options = test_options();
        options.device_name = pattern;
        const Device gpu{options};
        CHECK(gpu.report().name == name);
    }
}

// ---- Criterion 2: the device report; ADR 0011: it says whether validation is active ------------

TEST_CASE("device: the report gives name, driver, API version, ray query and validation", "[gpu]") {
    const std::unique_ptr<Device> gpu = make_device();
    const cs::gpu::DeviceReport& report = gpu->report();
    const std::string text = report.to_string();
    std::cout << text << '\n'; // for CI logs: which device and driver ran the GPU tests

    CHECK_FALSE(report.name.empty());
    CHECK_FALSE(report.driver.empty());
    CHECK_FALSE(report.driver_info.empty());
    CHECK(report.api_version >= VK_API_VERSION_1_3);

    SECTION("the fields are the chosen physical device's") {
        VkPhysicalDeviceVulkan12Properties properties12{};
        properties12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
        VkPhysicalDeviceProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &properties12;
        vkGetPhysicalDeviceProperties2(gpu->physical_device(), &properties);
        CHECK(report.name == std::string(properties.properties.deviceName));
        CHECK(report.driver == std::string(properties12.driverName));
        CHECK(report.driver_info == std::string(properties12.driverInfo));
        CHECK(report.api_version == properties.properties.apiVersion);

        std::uint32_t count = 0;
        REQUIRE(vkEnumerateDeviceExtensionProperties(gpu->physical_device(), nullptr, &count,
                                                     nullptr) == VK_SUCCESS);
        std::vector<VkExtensionProperties> extensions(count);
        REQUIRE(vkEnumerateDeviceExtensionProperties(gpu->physical_device(), nullptr, &count,
                                                     extensions.data()) == VK_SUCCESS);
        const bool ray_query =
            std::any_of(extensions.begin(), extensions.end(), [](const VkExtensionProperties& e) {
                return std::string(e.extensionName) == "VK_KHR_ray_query";
            });
        CHECK(report.ray_query == ray_query);
    }

    SECTION("to_string prints every field") {
        CAPTURE(text);
        CHECK_THAT(text, ContainsSubstring("device: " + report.name));
        CHECK_THAT(text, ContainsSubstring("driver: " + report.driver));
        CHECK_THAT(text, ContainsSubstring("Vulkan: " + version_string(report.api_version)));
        CHECK_THAT(text, ContainsSubstring(report.ray_query ? "ray query: yes" : "ray query: no"));
        CHECK_THAT(text,
                   ContainsSubstring(report.validation ? "validation: on" : "validation: off"));
    }

    // Only a debug build with CS_REQUIRE_GPU=1 requires the layer. Elsewhere this checks nothing,
    // but does not SKIP: a skipped section would mark the whole report test skipped, and the
    // report is what says "validation: off" when the layer is missing.
    if (validation_layer_required()) {
        SECTION("in a debug build with CS_REQUIRE_GPU=1 the validation layer is active") {
            CHECK(report.validation);
        }
    }
}

// ---- Criterion 3: a validation error reaches the handler (which, in tests, fails the test) -----

TEST_CASE("device: a known-invalid call reaches the validation handler", "[gpu]") {
    const std::unique_ptr<Device> gpu = make_device();
    if (!gpu->report().validation) {
        SKIP("the Khronos validation layer is not loaded");
    }

    constexpr const char* vuid = "VUID-VkBufferCreateInfo-size-00912"; // size must be > 0
    std::vector<ValidationMessage> messages;
    {
        // Replaces the harness's handler, which would fail this test on the expected error.
        HandlerGuard guard{[&messages](const ValidationMessage& m) { messages.push_back(m); }};

        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = 0; // invalid on purpose
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buffer = VK_NULL_HANDLE;
        const VkResult result = vkCreateBuffer(gpu->device(), &info, nullptr, &buffer);
        if (result == VK_SUCCESS && buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(gpu->device(), buffer, nullptr);
        }
        guard.restore(); // before any assertion, so a failing CHECK runs under the harness's
    }

    const auto found = std::find_if(messages.begin(), messages.end(), [&](const auto& m) {
        return m.severity == ValidationMessage::Severity::error &&
               m.kind == ValidationMessage::Kind::validation &&
               (m.id.find(vuid) != std::string::npos || m.text.find(vuid) != std::string::npos);
    });
    for (const ValidationMessage& m : messages) {
        UNSCOPED_INFO(cs::gpu::to_string(m));
    }
    REQUIRE(found != messages.end());
    CHECK_THAT(cs::gpu::to_string(*found), ContainsSubstring(vuid));
}

// Header (set_validation_handler, to_string(ValidationMessage)); needs no device.
TEST_CASE("device: set_validation_handler returns the previous handler, and a message prints its "
          "id and text",
          "[gpu]") {
    SECTION("set_validation_handler returns the previous handler") {
        int first_calls = 0;
        HandlerGuard first{[&first_calls](const ValidationMessage&) { ++first_calls; }};
        cs::gpu::ValidationHandler previous =
            cs::gpu::set_validation_handler([](const ValidationMessage&) {});
        // The handler set_validation_handler gave back is `first`'s; put it back before asserting.
        cs::gpu::ValidationHandler second = cs::gpu::set_validation_handler(previous);
        first.restore();
        REQUIRE(previous);
        previous(ValidationMessage{});
        CHECK(first_calls == 1);
        CHECK(second);
    }

    SECTION("to_string names the id and the text") {
        ValidationMessage message;
        message.severity = ValidationMessage::Severity::error;
        message.kind = ValidationMessage::Kind::validation;
        message.id = "VUID-vkCmdDraw-None-02700";
        message.text = "a pipeline is not bound";
        const std::string text = cs::gpu::to_string(message);
        CHECK_THAT(text, ContainsSubstring("VUID-vkCmdDraw-None-02700"));
        CHECK_THAT(text, ContainsSubstring("a pipeline is not bound"));
    }
}

// Header (set_validation_handler), issue M1-48: once set_validation_handler has returned, the
// handler it replaced is never called again, even while another thread's Vulkan calls report
// messages. Before, the callback copied the handler under a lock and called the copy after
// releasing it, so a replaced handler could still run, after its owner (cs-render's
// ValidationCounter, say) was gone.
//
// Each handler records its calls; once replaced, it is marked, and a call that finds the mark is
// late. Every handler sleeps in its call, after counting it, and each replacement waits until the
// handler it replaces has been called, so each replacement races a call that is still running: a
// set_validation_handler that did not wait for it would see a late call within a few
// replacements. The marks are atomics and every handler's record outlives the reporting thread,
// so a late call is counted, not undefined behaviour, in any build.
//
// Under ThreadSanitizer this also checks that the handler is read and replaced under one lock.
// The debug preset's ASan cannot see that, but it need not: the property is an order between two
// threads, which the marks observe directly, and that order is what the old code got wrong, with
// no data race for a sanitizer to find (it copied the handler under its lock).
//
// The messages come from vkSubmitDebugUtilsMessageEXT, which reaches the device's messenger as the
// validation layer's do: the layer reports any one message only a few times (its duplicate message
// limit), and this test needs thousands.
TEST_CASE("device: a validation handler is never called after set_validation_handler has "
          "replaced it",
          "[gpu]") {
    const std::unique_ptr<Device> gpu = make_device();
    if (!gpu->report().validation) {
        SKIP("the Khronos validation layer is not loaded, so the device has no debug messenger");
    }
    REQUIRE(vkSubmitDebugUtilsMessageEXT != nullptr);

    struct Record {
        std::atomic<int> calls{0};
        std::atomic<bool> replaced{false};
    };
    std::atomic<int> late_calls{0};
    const auto handler_for = [&late_calls](Record* record) {
        return [record, &late_calls](const ValidationMessage&) {
            record->calls.fetch_add(1);
            // Long enough that a replacement which does not wait for this call marks the record
            // before the call ends.
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            if (record->replaced.load()) {
                late_calls.fetch_add(1);
            }
        };
    };

    std::vector<std::unique_ptr<Record>> records;
    records.push_back(std::make_unique<Record>());
    // Replaces the harness's handler, which would abort on a message from another thread.
    HandlerGuard guard{handler_for(records.back().get())};

    std::atomic<bool> stop{false};
    const VkInstance instance = gpu->instance();
    std::thread reporter([&stop, instance] {
        VkDebugUtilsMessengerCallbackDataEXT data{};
        data.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT;
        data.pMessageIdName = "cs-test-handler-replaced";
        data.pMessage = "a message from another thread while the handler is replaced";
        while (!stop.load()) {
            vkSubmitDebugUtilsMessageEXT(instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
                                         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &data);
            // A pause in which a waiting set_validation_handler takes the lock: the lock is not
            // fair, and this thread would otherwise take it again before the waiter wakes.
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    const auto stop_reporter = [&stop, &reporter] {
        stop = true;
        if (reporter.joinable()) {
            reporter.join();
        }
    };
    // Before the guard and the records go, also when an assertion throws.
    const Cleanup on_exit{stop_reporter};

    constexpr int replacements = 300;
    // A bound on the wait for each call, only so that a broken messenger cannot hang the run.
    constexpr auto call_timeout = std::chrono::seconds(10);
    int replaced = 0;
    for (; replaced < replacements; ++replaced) {
        Record& current = *records.back();
        const auto deadline = std::chrono::steady_clock::now() + call_timeout;
        while (current.calls.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        if (current.calls.load() == 0) {
            break; // no message reached the handler; the REQUIRE below says so
        }
        records.push_back(std::make_unique<Record>());
        cs::gpu::set_validation_handler(handler_for(records.back().get()));
        current.replaced = true;
    }
    stop_reporter();
    guard.restore(); // before any assertion, so a failing CHECK runs under the harness's handler

    CAPTURE(replaced);
    REQUIRE(replaced == replacements);
    CHECK(late_calls.load() == 0);
}

// Header (gpu_required): the one reading of CS_REQUIRE_GPU, for the GPU tests, the integration
// tests, cs-render and the app; needs no device.
TEST_CASE("device: gpu_required says whether CS_REQUIRE_GPU is set, not empty and not 0", "[gpu]") {
    {
        const ScopedEnv env{"CS_REQUIRE_GPU", std::nullopt};
        CHECK_FALSE(cs::gpu::gpu_required());
    }
#ifndef _WIN32 // on Windows an empty value unsets the variable, which the case above covers
    {
        const ScopedEnv env{"CS_REQUIRE_GPU", std::string()};
        CHECK_FALSE(cs::gpu::gpu_required());
    }
#endif
    {
        const ScopedEnv env{"CS_REQUIRE_GPU", std::string("0")};
        CHECK_FALSE(cs::gpu::gpu_required());
    }
    {
        const ScopedEnv env{"CS_REQUIRE_GPU", std::string("1")};
        CHECK(cs::gpu::gpu_required());
    }
    {
        const ScopedEnv env{"CS_REQUIRE_GPU", std::string("yes")};
        CHECK(cs::gpu::gpu_required());
    }
}

// ---- ADR 0011: a missing validation layer is reported, and fails when required -----------------

// Disables the layer through VK_LOADER_LAYERS_DISABLE, which the Vulkan loader (1.3.234 or later)
// reads at every vkCreateInstance and vkEnumerateInstanceLayerProperties, so setting it in the
// middle of the process takes effect for the next device. A loader in an elevated process on
// Windows, as in CI's runner, ignores it (ADR 0010): the layer cannot be disabled there, and the
// sections that need it disabled skip.
TEST_CASE("device: without the validation layer, required throws and if_available reports "
          "validation off",
          "[gpu]") {
    // First a device as every test makes one, so that no device skips (or, under
    // CS_REQUIRE_GPU=1, fails) as usual rather than surfacing below as NoDeviceError. It also
    // loads the loader's own functions (volk), which loader_lists_validation_layer() calls.
    {
        const std::unique_ptr<Device> probe = make_device();
    }

    SECTION("Validation::off never loads the layer") {
        DeviceOptions options = test_options();
        options.validation = Validation::off;
        const Device gpu{options};
        CHECK_FALSE(gpu.report().validation);
        CHECK_THAT(gpu.report().to_string(), ContainsSubstring("validation: off"));
    }

    SECTION("disabled layer: Validation::required throws ValidationUnavailableError naming it") {
        const ScopedEnv env{"VK_LOADER_LAYERS_DISABLE", std::string("VK_LAYER_KHRONOS_validation")};
        skip_if_loader_ignores_layers_disable();
        DeviceOptions options = test_options();
        options.validation = Validation::required;
        CHECK_THROWS_MATCHES(Device{options}, ValidationUnavailableError,
                             MessageMatches(ContainsSubstring("VK_LAYER_KHRONOS_validation")));
    }

    SECTION("disabled layer: Validation::if_available runs and says validation is off") {
        const ScopedEnv env{"VK_LOADER_LAYERS_DISABLE", std::string("VK_LAYER_KHRONOS_validation")};
        skip_if_loader_ignores_layers_disable();
        DeviceOptions options = test_options();
        options.validation = Validation::if_available;
        const Device gpu{options};
        CHECK_FALSE(gpu.report().validation);
        CHECK_THAT(gpu.report().to_string(), ContainsSubstring("validation: off"));
    }
}

// ---- Header: one Device per process at a time -------------------------------------------------

TEST_CASE("device: only one Device may be alive at a time", "[gpu]") {
    std::unique_ptr<Device> first = make_device();

    CHECK_THROWS_AS(Device{test_options()}, std::logic_error);

    // The failed second construction left the first device working.
    CHECK(first->device() != VK_NULL_HANDLE);
    CHECK(first->wait_idle() == VK_SUCCESS);

    // Once the first is gone, another may be made.
    first.reset();
    const Device second{test_options()};
    CHECK(second.device() != VK_NULL_HANDLE);
}
