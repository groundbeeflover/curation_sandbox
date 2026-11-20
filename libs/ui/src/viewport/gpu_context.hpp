// SPDX-License-Identifier: Apache-2.0
//
// The application's Vulkan device, and a count of what the validation layer reports on it
// (ADR 0011, point 8): the status bar shows the count, or "validation: off" when the layer did not
// load, so that a run without the layer cannot read as a clean one.
//
// This header includes no Vulkan, so that the main window can hold the context: only the
// viewport adapter's sources do (CLAUDE.md).
#pragma once

#include <QObject>
#include <QString>

#include <cstddef>
#include <memory>

namespace cs::gpu {
class Device;
struct DeviceOptions;
} // namespace cs::gpu

namespace cs::ui {

// "validation: off" when the layer is not active, otherwise "validation: <count>".
[[nodiscard]] QString validation_status_text(bool active, std::size_t count);

// CS_REQUIRE_GPU is set, and neither empty nor "0" (as the GPU tests read it): no device, or in a
// debug build no validation layer, is then an error rather than something to live without.
[[nodiscard]] bool gpu_required();

// The device options of the app: presentation unless headless; the validation layer as in a GPU
// test (on in a debug build when installed, required there under CS_REQUIRE_GPU, off in release);
// CS_GPU_DEVICE chooses among devices (cs/gpu/device.hpp).
[[nodiscard]] gpu::DeviceOptions app_device_options(bool headless);

class GpuContext;

// The application's context: app_device_options(headless). Without a Vulkan device, nothing, with
// the reason in `problem`, unless gpu_required(), when it throws cs::gpu::NoDeviceError. Throws as
// cs::gpu::Device does for any other failure.
[[nodiscard]] std::unique_ptr<GpuContext> make_app_gpu_context(bool headless, QString& problem);

// Owns the Device, and counts the validation layer's warnings and errors while it lives. Messages
// of kind general (the loader describing its drivers) are printed on stderr but not counted, as
// in the GPU tests. It installs a process-wide handler (cs::gpu::set_validation_handler) before
// making the Device and puts the previous one back after destroying it, so that what the
// Device's destruction reports is heard. One per process, as Device is.
class GpuContext : public QObject {
    Q_OBJECT
  public:
    // Throws as cs::gpu::Device does.
    explicit GpuContext(const gpu::DeviceOptions& options);
    ~GpuContext() override;

    GpuContext(const GpuContext&) = delete;
    GpuContext& operator=(const GpuContext&) = delete;
    GpuContext(GpuContext&&) = delete;
    GpuContext& operator=(GpuContext&&) = delete;

    [[nodiscard]] const gpu::Device& device() const { return *device_; }
    // The layer is active on the instance (DeviceReport::validation).
    [[nodiscard]] bool validation_active() const;
    // Warnings and errors so far.
    [[nodiscard]] std::size_t validation_messages() const { return messages_; }
    [[nodiscard]] QString validation_status() const {
        return validation_status_text(validation_active(), messages_);
    }

  signals:
    // A warning or error was counted. Emitted on the thread that made the Vulkan call.
    void validation_message();

  private:
    struct PreviousHandler;
    std::unique_ptr<PreviousHandler> previous_; // the validation handler to put back
    std::size_t messages_ = 0;
    std::unique_ptr<gpu::Device> device_;
};

} // namespace cs::ui
