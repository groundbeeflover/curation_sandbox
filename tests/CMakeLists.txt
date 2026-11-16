# SPDX-License-Identifier: Apache-2.0
# Tests that belong to no single module. Each module's own tests live in libs/<m>/tests/ and are
# added by cs_add_module. Every tests/<dir>/ with a CMakeLists.txt is added too, as
# tests/integration/ (cs_integration_tests, cmake/CsIntegration.cmake) is.
cs_add_subdirectories("${CMAKE_CURRENT_SOURCE_DIR}")

# cs_add_module itself: a fixture project built in the build tree (cmake/tests/module_glob.cmake).
add_test(NAME cmake_module_glob
    COMMAND ${CMAKE_COMMAND}
        -D "CS_REPO_DIR=${PROJECT_SOURCE_DIR}"
        -D "WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cmake_module_glob"
        -D "GENERATOR=${CMAKE_GENERATOR}"
        -D "CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        -D "CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}"
        -D "BUILD_TYPE=$<CONFIG>"
        -D "Catch2_DIR=${Catch2_DIR}"
        -D "CTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
        -P "${PROJECT_SOURCE_DIR}/cmake/tests/module_glob.cmake")
set_tests_properties(cmake_module_glob PROPERTIES LABELS cmake TIMEOUT 600)

# cs_generate_registry: a fixture project built in the build tree (cmake/tests/registry.cmake).
add_test(NAME cmake_registry
    COMMAND ${CMAKE_COMMAND}
        -D "CS_REPO_DIR=${PROJECT_SOURCE_DIR}"
        -D "WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cmake_registry"
        -D "GENERATOR=${CMAKE_GENERATOR}"
        -D "CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        -D "CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}"
        -D "BUILD_TYPE=$<CONFIG>"
        -P "${PROJECT_SOURCE_DIR}/cmake/tests/registry.cmake")
set_tests_properties(cmake_registry PROPERTIES LABELS cmake TIMEOUT 600)
