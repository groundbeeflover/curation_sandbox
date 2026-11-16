// SPDX-License-Identifier: Apache-2.0

// Validation messages fail the running test in cs_integration_tests (issue #78, M1-22), as
// libs/gpu/tests/main.cpp makes them do in the gpu tests. This executable has Catch2's own main,
// so the hook is a Catch2 event listener, registered once here for every GPU integration test.
//
// At the start of the run it installs a validation handler. A warning or error from the
// validation layer that arrives on the test thread inside a test fails that test with FAIL_CHECK,
// and the test goes on, so one run shows every message. Anywhere else (another thread, before or
// after a test) Catch2 cannot attribute it to a test, so the message is printed and the process
// aborts: the run still fails, with the message in its output. Messages of kind general (the
// loader describing its drivers and layers) are printed and fail nothing.
//
// Devices come from libs/gpu/tests/gpu_test.hpp, included by its relative path: make_device()
// skips without a device, fails without one under CS_REQUIRE_GPU=1, and requires the validation
// layer under CS_REQUIRE_GPU=1 in a debug build (ADR 0010, ADR 0011).
//
// A test may install its own handler for a while (test_skeleton_gpu_scene.cpp's collector does);
// it puts this one back when it is done.

#include "../../libs/gpu/tests/gpu_test.hpp"

#include <cs/gpu/device.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

std::atomic<bool> in_test_case{false};
std::thread::id test_thread;

void fail_on_validation(const cs::gpu::ValidationMessage& message) {
    using Message = cs::gpu::ValidationMessage;
    const std::string text = cs::gpu::to_string(message);
    const bool fails =
        message.severity >= Message::Severity::warning && message.kind != Message::Kind::general;
    if (!fails) {
        std::cerr << text << '\n';
        return;
    }
    if (in_test_case && std::this_thread::get_id() == test_thread) {
        FAIL_CHECK(text);
        return;
    }
    std::cerr << text
              << "\nThis validation message arrived outside a test or on another thread, "
                 "so it cannot fail one test: aborting.\n";
    std::abort();
}

class GpuValidationListener : public Catch::EventListenerBase {
  public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo&) override {
        test_thread = std::this_thread::get_id();
        cs::gpu::set_validation_handler(fail_on_validation);
    }
    void testCaseStarting(const Catch::TestCaseInfo&) override { in_test_case = true; }
    void testCaseEnded(const Catch::TestCaseStats&) override { in_test_case = false; }
};

} // namespace

CATCH_REGISTER_LISTENER(GpuValidationListener)

// The listener is active: a known-invalid call fails the test that makes it. [!shouldfail]
// turns that failure into a pass, and a pass (no hook, or a hook that lets the message by) into a
// failure. So nothing else may fail this test: a device or validation layer that is missing makes
// it skip, never fail, even under CS_REQUIRE_GPU=1 (make_device() would FAIL there, which
// [!shouldfail] would report as a pass). In that case every other GPU test fails the run instead.
TEST_CASE("gpu validation: a known-invalid Vulkan call fails the running test",
          "[integration][gpu][!shouldfail]") {
    std::unique_ptr<cs::gpu::Device> gpu;
    try {
        gpu = std::make_unique<cs::gpu::Device>(cs::gpu::test::test_options());
    } catch (const cs::gpu::NoDeviceError& e) {
        SKIP("no Vulkan device: " << e.what());
    } catch (const cs::gpu::ValidationUnavailableError& e) {
        SKIP("the validation layer is required but missing: " << e.what());
    }
    if (!gpu->report().validation) {
        SKIP("the Khronos validation layer is not loaded, so no call can be reported");
    }
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = 0; // invalid on purpose: VUID-VkBufferCreateInfo-size-00912
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    const VkResult result = vkCreateBuffer(gpu->device(), &info, nullptr, &buffer);
    if (result == VK_SUCCESS && buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(gpu->device(), buffer, nullptr);
    }
}
