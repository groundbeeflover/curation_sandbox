// SPDX-License-Identifier: Apache-2.0
//
// The gpu tests' main: Catch2's, plus a validation handler that turns every validation error or
// warning into a failure of the test that caused it (ADR 0001), with the layer's message.
//
// Validation messages arrive on the thread that made the Vulkan call, while it runs. On the test
// thread, inside a test, the message fails that test with FAIL_CHECK and the test goes on, so
// one run shows every message. Anywhere else (another thread, before or after a test) Catch2
// cannot attribute it to a test, so the message is printed and the process aborts: the test
// still fails, and the message is in its output.
//
// Messages of kind general (the loader describing which drivers and layers it found) are printed
// but fail nothing: they are not validation.

#include <cs/gpu/device.hpp>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

std::atomic<bool> in_test_case{false};
std::thread::id test_thread;

class TestCaseTracker : public Catch::EventListenerBase {
  public:
    using Catch::EventListenerBase::EventListenerBase;
    void testCaseStarting(const Catch::TestCaseInfo&) override { in_test_case = true; }
    void testCaseEnded(const Catch::TestCaseStats&) override { in_test_case = false; }
};

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

// The Vulkan loader unloads a driver when its instance is destroyed. Mesa's drivers keep memory
// they allocate once per process in their own static data, so after unloading LeakSanitizer
// reports it as leaked, from frames that have lost their names. Keeping the libraries loaded
// keeps that memory reachable. Set before the first Vulkan call, unless already set.
void keep_vulkan_libraries_loaded() {
    constexpr const char* name = "VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING";
#ifdef _WIN32
    std::size_t length = 0;
    if (getenv_s(&length, nullptr, 0, name) == 0 && length == 0)
        _putenv_s(name, "1");
#else
    setenv(name, "1", 0);
#endif
}

} // namespace

CATCH_REGISTER_LISTENER(TestCaseTracker)

int main(int argc, char* argv[]) {
    test_thread = std::this_thread::get_id();
    keep_vulkan_libraries_loaded();
    cs::gpu::set_validation_handler(fail_on_validation);
    return Catch::Session().run(argc, argv);
}
