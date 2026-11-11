// SPDX-License-Identifier: Apache-2.0
#include <cs/alpha/alpha.hpp>
#include <cs/beta/beta.hpp>

#include <catch2/catch_test_macros.hpp>

extern bool g_custom_main_ran;

TEST_CASE("beta builds on alpha", "[beta-tag]") {
    CHECK(cs::beta::value() == cs::alpha::value() + 1);
}

TEST_CASE("the module's own main runs", "[beta-tag]") {
    CHECK(g_custom_main_ran);
}

TEST_CASE("a skipped test is reported as skipped", "[beta-tag]") {
    SKIP("skipped on purpose");
}
