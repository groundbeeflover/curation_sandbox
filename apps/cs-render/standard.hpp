// SPDX-License-Identifier: Apache-2.0

#pragma once

// What cs-render needs of the GPU: a Vulkan device made as the GPU tests make theirs, the name of
// its golden references, a count of its validation messages, and the first viewpoint rendered
// through the Standard tier (cs/gpu/standard_tier.hpp).

#include "common.hpp"

#include <cs/core/image.hpp>
#include <cs/gpu/device.hpp>

#include <atomic>
#include <cstddef>
#include <string>

namespace cs_render {

// CS_REQUIRE_GPU=1 (CI and /ship set it): set, and neither empty nor "0", as the GPU tests read
// it (libs/gpu/tests/gpu_test.hpp). No device, or in a debug build no validation layer, is then
// a failure rather than a skip.
bool gpu_required();

// Headless; the validation layer as in a GPU test (ADR 0011): required in a debug build under
// CS_REQUIRE_GPU=1, otherwise cs::gpu::default_validation (on in a debug build when installed,
// off in release). CS_GPU_DEVICE chooses among several devices (device.hpp).
cs::gpu::DeviceOptions device_options();

// The name a device's golden references go by, ref.<name>.exr (tests/golden/README.md):
// "lavapipe" for Mesa's llvmpipe, otherwise the driver's name (VkPhysicalDeviceVulkan12Properties::
// driverName, "radv" for Mesa's RADV) in lower case, with each run of other characters than a-z
// and 0-9 made one '-', none at either end, and at most 64 characters. "unknown" if that leaves
// nothing.
std::string driver_label(const cs::gpu::DeviceReport& report);

// Counts the warnings and errors the validation layer reports while it lives, and prints each on
// stderr as it arrives. Messages of kind general (the loader describing its drivers and layers)
// are printed but not counted, as in the GPU tests (tests/integration/gpu_validation.cpp). It
// installs a process-wide handler (cs::gpu::set_validation_handler) and puts the previous one
// back; make it before the Device and let it outlive the Device, whose destruction can report
// objects left alive.
class ValidationCounter {
  public:
    ValidationCounter();
    ~ValidationCounter();

    ValidationCounter(const ValidationCounter&) = delete;
    ValidationCounter& operator=(const ValidationCounter&) = delete;
    ValidationCounter(ValidationCounter&&) = delete;
    ValidationCounter& operator=(ValidationCounter&&) = delete;

    [[nodiscard]] std::size_t count() const { return count_.load(); }

  private:
    std::atomic<std::size_t> count_{0};
    cs::gpu::ValidationHandler previous_;
};

// The first viewpoint through the Standard tier, width x height: linear Rec. 709 radiance in nits,
// not exposed or tone mapped (StandardImages::radiance). The paintings' photographs come from the
// project's assets/; one the GPU scene cannot show is a warning on stderr, as load_project's are.
// Throws as cs::gpu::Scene and cs::gpu::StandardTier do.
cs::Image render_standard(const cs::gpu::Device& device, const Project& project, int width,
                          int height);

} // namespace cs_render
