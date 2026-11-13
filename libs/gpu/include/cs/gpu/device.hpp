// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Vulkan 1.3 device every GPU tier renders with: an instance, one physical device, a logical
// device with one graphics-and-compute queue, and a VMA allocator. It runs headless (tests, the
// command-line renderer) or ready for a window whose surface is created later (the app).
//
// Vulkan functions come from volk, which loads them at run time into process-wide pointers, so
// there is one Device per process at a time: the constructor throws std::logic_error while
// another is alive. Include <volk.h>, never <vulkan/vulkan.h>, to call Vulkan.

#include <volk.h>

#include <vk_mem_alloc.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cs::gpu {

// Whether the Khronos validation layer (VK_LAYER_KHRONOS_validation) is loaded.
enum class Validation {
    off,          // never
    if_available, // when it is installed; DeviceReport::validation says whether it was
    required,     // or the constructor throws ValidationUnavailableError
};

// On in debug builds when installed, off in release builds (ADR 0001).
#ifdef NDEBUG
inline constexpr Validation default_validation = Validation::off;
#else
inline constexpr Validation default_validation = Validation::if_available;
#endif

struct DeviceOptions {
    Validation validation = default_validation;
    // Ready to present: enables VK_KHR_surface and the window system's surface extensions on
    // the instance, and VK_KHR_swapchain on the device. The surface itself is created later
    // from instance(), by whoever owns the window; can_present() checks it.
    bool presentation = false;
    // Chooses the device whose name contains this, ignoring case. When unset, the environment
    // variable CS_GPU_DEVICE does the same; when neither is set, a discrete GPU is preferred,
    // then an integrated one, then any other (lavapipe is a CPU device). An empty string means
    // no choice, ignoring CS_GPU_DEVICE. A name longer than 256 characters is a mistake, not a
    // missing device: it throws std::invalid_argument.
    std::optional<std::string> device_name;
    // Instance extensions enabled when the loader offers them, and skipped silently otherwise;
    // has_instance_extension() says which were enabled.
    std::vector<std::string> optional_instance_extensions;
};

// What was chosen, for logs and CI: printed by to_string() as one line per field.
struct DeviceReport {
    std::string name;              // VkPhysicalDeviceProperties::deviceName, "llvmpipe (LLVM ...)"
    std::string driver;            // VkPhysicalDeviceVulkan12Properties::driverName, "llvmpipe"
    std::string driver_info;       // driverInfo, "Mesa 26.2.6 (LLVM ...)"
    std::uint32_t api_version = 0; // the device's VK_MAKE_API_VERSION
    bool ray_query = false;        // VK_KHR_ray_query is available (not enabled by this device)
    bool validation = false;       // the Khronos validation layer is active on the instance

    // "device: ...\ndriver: ...\nVulkan: 1.4.335\nray query: yes\nvalidation: on"
    [[nodiscard]] std::string to_string() const;
};

// A message from the validation layer or the loader, through VK_EXT_debug_utils.
struct ValidationMessage {
    enum class Severity { verbose, info, warning, error };
    enum class Kind { general, validation, performance };
    Severity severity = Severity::error;
    Kind kind = Kind::validation; // general messages come from the loader and layer setup
    std::string id;               // the VUID or message name, may be empty
    std::string text;
};

// "validation error VUID-...: text", for logs and test failures.
[[nodiscard]] std::string to_string(const ValidationMessage& message);

// Receives every warning and error the instance reports. It is called on the thread that made
// the Vulkan call, while the call runs. Process-wide; installing one returns the previous one.
// Without one (nullptr), messages are printed to stderr. The test harness installs one that
// fails the running test.
using ValidationHandler = std::function<void(const ValidationMessage&)>;
ValidationHandler set_validation_handler(ValidationHandler handler);

// No Vulkan loader, no driver, or no device with Vulkan 1.3 and the features below (or none
// matching device_name / CS_GPU_DEVICE). GPU tests skip on this, or fail with CS_REQUIRE_GPU=1.
class NoDeviceError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// Validation::required, but the validation layer is not installed or was disabled.
class ValidationUnavailableError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// Vulkan 1.3 with dynamic rendering, synchronization2 and buffer device address enabled, and a
// VMA allocator that can allocate buffers with device addresses.
class Device {
  public:
    // Throws NoDeviceError, ValidationUnavailableError, std::logic_error (another Device is
    // alive), std::invalid_argument (an over-long device name) or std::runtime_error (any other
    // Vulkan failure).
    explicit Device(const DeviceOptions& options = {});
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;

    [[nodiscard]] VkInstance instance() const { return instance_; }
    [[nodiscard]] VkPhysicalDevice physical_device() const { return physical_device_; }
    [[nodiscard]] VkDevice device() const { return device_; }
    // One queue that supports graphics, compute and transfer.
    [[nodiscard]] VkQueue queue() const { return queue_; }
    [[nodiscard]] std::uint32_t queue_family() const { return queue_family_; }
    [[nodiscard]] VmaAllocator allocator() const { return allocator_; }

    [[nodiscard]] const DeviceReport& report() const { return report_; }
    [[nodiscard]] bool presentation() const { return has_extension("VK_KHR_swapchain"); }
    // Whether a device or instance extension was enabled, such as "VK_KHR_swapchain".
    [[nodiscard]] bool has_extension(std::string_view name) const;
    [[nodiscard]] bool has_instance_extension(std::string_view name) const;
    // Whether the queue can present to a surface created from instance() after construction.
    // False on a headless device.
    [[nodiscard]] bool can_present(VkSurfaceKHR surface) const;

    void wait_idle() const;

  private:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    DeviceReport report_;
    std::vector<std::string> extensions_;
    std::vector<std::string> instance_extensions_;

    void destroy() noexcept;
};

} // namespace cs::gpu
