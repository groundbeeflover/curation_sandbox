// SPDX-License-Identifier: Apache-2.0
#include "viewport/gpu_context.hpp"

#include <cs/gpu/device.hpp>

#include <iostream>
#include <utility>

namespace cs::ui {

QString validation_status_text(bool active, std::size_t count) {
    if (!active) {
        return QStringLiteral("validation: off");
    }
    return QStringLiteral("validation: %1").arg(static_cast<qulonglong>(count));
}

bool gpu_required() {
    const QString value = qEnvironmentVariable("CS_REQUIRE_GPU");
    return !value.isEmpty() && value != QLatin1String("0");
}

gpu::DeviceOptions app_device_options(bool headless) {
    gpu::DeviceOptions options;
    options.presentation = !headless;
#ifndef NDEBUG
    if (gpu_required()) {
        options.validation = gpu::Validation::required;
    }
#endif
    return options;
}

std::unique_ptr<GpuContext> make_app_gpu_context(bool headless, QString& problem) {
    try {
        return std::make_unique<GpuContext>(app_device_options(headless));
    } catch (const gpu::NoDeviceError& error) {
        if (gpu_required()) {
            throw;
        }
        problem = QString::fromStdString(error.what());
        std::cerr << "curation: no viewport: " << error.what() << '\n';
        return nullptr;
    }
}

struct GpuContext::PreviousHandler {
    gpu::ValidationHandler handler;
};

GpuContext::GpuContext(const gpu::DeviceOptions& options)
    : previous_(std::make_unique<PreviousHandler>()) {
    previous_->handler = gpu::set_validation_handler([this](const gpu::ValidationMessage& message) {
        using Message = gpu::ValidationMessage;
        std::cerr << gpu::to_string(message) << '\n';
        if (message.severity >= Message::Severity::warning &&
            message.kind != Message::Kind::general) {
            ++messages_;
            emit validation_message();
        }
    });
    try {
        device_ = std::make_unique<gpu::Device>(options);
    } catch (...) {
        gpu::set_validation_handler(std::move(previous_->handler));
        throw;
    }
}

GpuContext::~GpuContext() {
    // What the device's destruction reports is still counted (and printed).
    device_.reset();
    gpu::set_validation_handler(std::move(previous_->handler));
}

bool GpuContext::validation_active() const {
    return device_->report().validation;
}

} // namespace cs::ui
