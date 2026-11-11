// SPDX-License-Identifier: Apache-2.0
#include <cs/alpha/alpha.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

TEST_CASE("alpha returns one", "[alpha-tag]") {
    CHECK(cs::alpha::value() == 1);
}

TEST_CASE("CS_SOURCE_DIR is the root of the project", "[alpha-tag]") {
    CHECK(std::filesystem::exists(std::filesystem::path(CS_SOURCE_DIR) /
                                  "libs/alpha/CMakeLists.txt"));
}
