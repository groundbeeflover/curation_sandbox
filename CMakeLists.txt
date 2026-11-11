# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.25)

# ---- vcpkg -------------------------------------------------------------------
# Dependencies come from vcpkg in manifest mode (vcpkg.json, ADR 0001). The presets point
# CMAKE_TOOLCHAIN_FILE at $VCPKG_ROOT; without a toolchain, $VCPKG_ROOT is used directly.
set(cs_vcpkg_help [=[
vcpkg was not found: set VCPKG_ROOT to a vcpkg checkout. To install vcpkg once:
    git clone https://github.com/microsoft/vcpkg ~/vcpkg
    ~/vcpkg/bootstrap-vcpkg.sh -disableMetrics      (Windows: bootstrap-vcpkg.bat)
    export VCPKG_ROOT=~/vcpkg                       (and add this line to your shell profile)
Then configure again, for example with `cmake --preset debug`.]=])
if(NOT CMAKE_TOOLCHAIN_FILE)
    if("$ENV{VCPKG_ROOT}" STREQUAL "")
        message(FATAL_ERROR "${cs_vcpkg_help}")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "")
endif()
if(CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg\\.cmake$" AND NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    message(FATAL_ERROR "${cs_vcpkg_help}\n(looked for ${CMAKE_TOOLCHAIN_FILE})")
endif()

project(CurationSandbox
    VERSION 0.1.0
    DESCRIPTION "A digital maquette for exhibitions, with physically accurate light"
    LANGUAGES CXX)

# ---- Helpers -----------------------------------------------------------------
# Every cmake/Cs*.cmake is included, so a task adds a helper without editing this file. The
# options come first, so every other helper can read them.
include("${PROJECT_SOURCE_DIR}/cmake/CsOptions.cmake")
file(GLOB cs_helpers CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/cmake/Cs*.cmake")
list(REMOVE_ITEM cs_helpers "${PROJECT_SOURCE_DIR}/cmake/CsOptions.cmake")
foreach(helper IN LISTS cs_helpers)
    include("${helper}")
endforeach()

# ---- Project parts -----------------------------------------------------------
# Every module under libs/ (blueprint section 7). Modules may name each other in any order;
# links are resolved once all of them are configured.
cs_add_subdirectories("${PROJECT_SOURCE_DIR}/libs")
add_subdirectory(apps)

if(CS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
