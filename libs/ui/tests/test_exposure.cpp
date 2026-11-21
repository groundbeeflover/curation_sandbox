// SPDX-License-Identifier: Apache-2.0
//
// The viewport's exposure and the validation status text (issue #82): pure rules, no GPU.

#include "ui_test.hpp"
#include "viewport/exposure.hpp"
#include "viewport/gpu_context.hpp"

#include <cs/gpu/device.hpp>
#include <cs/world/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <QString>

#include <limits>
#include <optional>
#include <string>

namespace {

using cs::ui::Exposure;

cs::world::Camera viewpoint_with(std::optional<double> ev100) {
    cs::world::Camera camera;
    camera.vertical_fov_rad = 0.8726646259971648; // 50 degrees, as the skeleton's viewpoint
    camera.ev100 = ev100;
    return camera;
}

} // namespace

// Acceptance criterion: EV100 8 for a new project. Values compared exactly: they are copied or
// clamped, never computed.
TEST_CASE("Exposure: a new project starts at EV100 8", "[ui]") {
    STATIC_CHECK(Exposure::default_ev100 == 8.0);
    STATIC_CHECK(Exposure::max_ev100_magnitude == 100.0);
    const Exposure exposure;
    CHECK(exposure.ev100() == 8.0);
}

// Acceptance criterion: the viewport uses the active viewpoint's EV100.
TEST_CASE("Exposure: a viewpoint's EV100 is used", "[ui]") {
    Exposure exposure;
    exposure.view_from(viewpoint_with(6.5));
    CHECK(exposure.ev100() == 6.5);
    exposure.view_from(viewpoint_with(-3.25));
    CHECK(exposure.ev100() == -3.25);
    exposure.view_from(viewpoint_with(0.0));
    CHECK(exposure.ev100() == 0.0);
}

// Acceptance criterion: with no viewpoint EV100 (automatic exposure, not in M1) it keeps the last.
TEST_CASE("Exposure: a viewpoint without an EV100 keeps the last one", "[ui]") {
    Exposure exposure;
    exposure.view_from(viewpoint_with(std::nullopt));
    CHECK(exposure.ev100() == 8.0);
    exposure.view_from(viewpoint_with(6.5));
    exposure.view_from(viewpoint_with(std::nullopt));
    CHECK(exposure.ev100() == 6.5);
}

// Acceptance criterion (EV100 kept within +-100 for the tone-mapping pass; untrusted values fail
// cleanly): a value that is not finite keeps the last one, one beyond the range is clamped.
TEST_CASE("Exposure: a non-finite EV100 keeps the last; one beyond +-100 is clamped", "[ui]") {
    Exposure exposure;
    exposure.view_from(viewpoint_with(6.5));

    exposure.view_from(viewpoint_with(std::numeric_limits<double>::quiet_NaN()));
    CHECK(exposure.ev100() == 6.5);
    exposure.view_from(viewpoint_with(std::numeric_limits<double>::infinity()));
    CHECK(exposure.ev100() == 6.5);
    exposure.view_from(viewpoint_with(-std::numeric_limits<double>::infinity()));
    CHECK(exposure.ev100() == 6.5);

    exposure.view_from(viewpoint_with(100.0));
    CHECK(exposure.ev100() == 100.0);
    exposure.view_from(viewpoint_with(100.5));
    CHECK(exposure.ev100() == 100.0);
    exposure.view_from(viewpoint_with(std::numeric_limits<double>::max()));
    CHECK(exposure.ev100() == 100.0);
    exposure.view_from(viewpoint_with(-100.0));
    CHECK(exposure.ev100() == -100.0);
    exposure.view_from(viewpoint_with(-1e9));
    CHECK(exposure.ev100() == -100.0);
}

// Acceptance criterion: a new project goes back to EV100 8.
TEST_CASE("Exposure: reset() goes back to EV100 8", "[ui]") {
    Exposure exposure;
    exposure.view_from(viewpoint_with(-42.0));
    exposure.reset();
    CHECK(exposure.ev100() == 8.0);
}

// Acceptance criterion: the status shows the validation message count, or "validation: off" when
// the layer did not load (ADR 0011, point 8).
TEST_CASE("validation_status_text: the count, or off when the layer is not active", "[ui]") {
    CHECK(cs::ui::validation_status_text(false, 0) == QStringLiteral("validation: off"));
    CHECK(cs::ui::validation_status_text(false, 7) == QStringLiteral("validation: off"));
    CHECK(cs::ui::validation_status_text(true, 0) == QStringLiteral("validation: 0"));
    CHECK(cs::ui::validation_status_text(true, 3) == QStringLiteral("validation: 3"));
    CHECK(cs::ui::validation_status_text(true, 12345) == QStringLiteral("validation: 12345"));
}

// Acceptance criterion (UI tests that claim zero messages fail when the layer is off): the app
// reads CS_REQUIRE_GPU as the GPU tests do.
TEST_CASE("gpu_required: CS_REQUIRE_GPU set, not empty and not 0", "[ui]") {
    using cs::ui::test::ScopedEnv;
    {
        const ScopedEnv env("CS_REQUIRE_GPU", std::nullopt);
        CHECK_FALSE(cs::ui::gpu_required());
    }
#ifndef _WIN32 // on Windows an empty value unsets the variable, which the case above covers
    {
        const ScopedEnv env("CS_REQUIRE_GPU", std::string());
        CHECK_FALSE(cs::ui::gpu_required());
    }
#endif
    {
        const ScopedEnv env("CS_REQUIRE_GPU", std::string("0"));
        CHECK_FALSE(cs::ui::gpu_required());
    }
    {
        const ScopedEnv env("CS_REQUIRE_GPU", std::string("1"));
        CHECK(cs::ui::gpu_required());
    }
}

// Acceptance criterion (headless viewport; validation as in the GPU tests): the app's device
// options present only with a window surface, and use the layer as a GPU test does.
TEST_CASE("app_device_options: presentation unless headless; validation as in the GPU tests",
          "[ui]") {
    using cs::ui::test::ScopedEnv;
    CHECK_FALSE(cs::ui::app_device_options(true).presentation);
    CHECK(cs::ui::app_device_options(false).presentation);

    {
        const ScopedEnv env("CS_REQUIRE_GPU", std::string("0"));
#ifdef NDEBUG
        CHECK(cs::ui::app_device_options(true).validation == cs::gpu::Validation::off);
#else
        CHECK(cs::ui::app_device_options(true).validation == cs::gpu::Validation::if_available);
#endif
    }
    {
        const ScopedEnv env("CS_REQUIRE_GPU", std::string("1"));
#ifdef NDEBUG
        CHECK(cs::ui::app_device_options(true).validation == cs::gpu::Validation::off);
#else
        CHECK(cs::ui::app_device_options(true).validation == cs::gpu::Validation::required);
#endif
    }
}
