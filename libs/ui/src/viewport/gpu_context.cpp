// SPDX-License-Identifier: Apache-2.0
#include "viewport/gpu_context.hpp"

#include <cs/gpu/device.hpp>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

namespace cs::ui {

namespace {

std::optional<std::string> environment_variable(const char* name) {
#ifdef _WIN32
    // MSVC deprecates getenv (an error under /WX).
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
#endif
}

} // namespace

QString validation_status_text(bool active, std::size_t count) {
    if (!active) {
        return QStringLiteral("validation: off");
    }
    return QStringLiteral("validation: %1").arg(static_cast<qulonglong>(count));
}

bool gpu_required() {
    const std::optional<std::string> value = environment_variable("CS_REQUIRE_GPU");
    return value && !value->empty() && *value != "0";
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
