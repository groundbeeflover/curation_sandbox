# SPDX-License-Identifier: Apache-2.0
# A module with a generated registry, built with the repository's cs_add_module and
# cs_generate_registry, driven by ../registry.cmake. It is copied into the build tree before it is
# configured.
cmake_minimum_required(VERSION 3.25)
project(CsRegistryFixture LANGUAGES CXX)

include("${CS_REPO_DIR}/cmake/CsOptions.cmake")
include("${CS_REPO_DIR}/cmake/CsModule.cmake")
include("${CS_REPO_DIR}/cmake/CsRegistry.cmake")

cs_add_subdirectories("${PROJECT_SOURCE_DIR}/libs")

# Prints the greetings that register_greetings registers, one per line.
add_executable(list_greetings list_greetings.cpp)
target_link_libraries(list_greetings PRIVATE cs::greet cs_options)
