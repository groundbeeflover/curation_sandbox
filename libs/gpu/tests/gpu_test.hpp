// SPDX-License-Identifier: Apache-2.0
#pragma once

// What every gpu test file shares: make_device(), which skips without a device and fails without
// one under CS_REQUIRE_GPU=1 (ADR 0010); the options that require the validation layer in a debug
// build under CS_REQUIRE_GPU=1 (ADR 0011); and scope guards for environment variables, validation
// handlers and Vulkan objects. The harness in main.cpp fails the running test on any validation
// warning or error.
//
// Only one cs::gpu::Device may be alive at a time, so every test scopes its devices so that they
// never overlap.

#include <cs/gpu/device.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace cs::gpu::test {

#ifdef NDEBUG
inline constexpr bool debug_build = false;
#else
inline constexpr bool debug_build = true;
#endif

// ---- Environment -------------------------------------------------------------------------------

// The value of an environment variable, or nothing when it is unset. MSVC deprecates getenv
// (a warning, an error under /WX), so Windows reads through _dupenv_s.
inline std::optional<std::string> read_env(const char* name) {
#ifdef _WIN32
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
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

// Sets (or, with nothing, unsets) an environment variable. On Windows an empty value unsets it.
inline void write_env(const char* name, const std::optional<std::string>& value) {
#ifdef _WIN32
    _putenv_s(name, value ? value->c_str() : "");
#else
    if (value) {
        setenv(name, value->c_str(), 1);
    } else {
        unsetenv(name);
    }
#endif
}

// Sets an environment variable for one scope and restores its previous value (or its absence)
// on the way out, also when an assertion throws.
class ScopedEnv {
  public:
    ScopedEnv(std::string name, const std::optional<std::string>& value)
        : name_(std::move(name)), previous_(read_env(name_.c_str())) {
        write_env(name_.c_str(), value);
    }
    ~ScopedEnv() { write_env(name_.c_str(), previous_); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

  private:
    std::string name_;
    std::optional<std::string> previous_;
};

// ---- Devices -----------------------------------------------------------------------------------

// CI and /ship set CS_REQUIRE_GPU=1: then no device (or, in debug, no validation layer) is a
// failure rather than a skip. Read by gpu itself (device.hpp), as everything else that asks does.
using cs::gpu::gpu_required;

// In a debug build under CS_REQUIRE_GPU=1 the validation layer must load (ADR 0011). A
// function, not an `if` on debug_build: MSVC warns (C4127) on a constant in a condition.
inline bool validation_layer_required() {
    return debug_build && gpu_required();
}

// The options every test starts from: the layer required as above, otherwise used when
// installed.
inline DeviceOptions test_options() {
    DeviceOptions options;
    options.validation =
        validation_layer_required() ? Validation::required : Validation::if_available;
    return options;
}

// A device, or the test skips (no device) or fails (no device under CS_REQUIRE_GPU=1).
// ValidationUnavailableError is not caught: it fails the test with its own message.
// The device is returned from after the handler, on the path that made it: FAIL and SKIP throw,
// so a return after them alone is unreachable (MSVC's warning C4702 in debug builds), while GCC
// and Clang cannot always tell that they throw and want a return on every path.
inline std::unique_ptr<Device> make_device(const DeviceOptions& options = test_options()) {
    std::unique_ptr<Device> device;
    try {
        device = std::make_unique<Device>(options);
    } catch (const NoDeviceError& e) {
        if (gpu_required()) {
            FAIL("no Vulkan device was found, and CS_REQUIRE_GPU=1 requires one: " << e.what());
        } else {
            SKIP("no Vulkan device was found: " << e.what());
        }
    }
    return device;
}

// ---- Scope guards ------------------------------------------------------------------------------

// Runs a function when the scope ends, so that Vulkan objects are destroyed before the device
// even when an assertion throws: a leaked object is a validation error at device destruction.
template <typename F> class Cleanup {
  public:
    explicit Cleanup(F f) : f_(std::move(f)) {}
    ~Cleanup() { f_(); }
    Cleanup(const Cleanup&) = delete;
    Cleanup& operator=(const Cleanup&) = delete;

  private:
    F f_;
};

// Installs a validation handler for one scope and puts the previous one back, on restore() or
// on the way out, whichever comes first.
class HandlerGuard {
  public:
    explicit HandlerGuard(ValidationHandler handler)
        : previous_(set_validation_handler(std::move(handler))) {}
    ~HandlerGuard() { restore(); }
    HandlerGuard(const HandlerGuard&) = delete;
    HandlerGuard& operator=(const HandlerGuard&) = delete;

    void restore() {
        if (!restored_) {
            set_validation_handler(std::move(previous_));
            restored_ = true;
        }
    }

  private:
    ValidationHandler previous_;
    bool restored_ = false;
};

} // namespace cs::gpu::test
