# SPDX-License-Identifier: Apache-2.0
# A small project of four modules built with the repository's cs_add_module, driven by
# ../module_glob.cmake. It is copied into the build tree before it is configured.
cmake_minimum_required(VERSION 3.25)
project(CsModuleGlobFixture LANGUAGES CXX)

include("${CS_REPO_DIR}/cmake/CsOptions.cmake")
include("${CS_REPO_DIR}/cmake/CsModule.cmake")

cs_add_subdirectories("${PROJECT_SOURCE_DIR}/libs")
