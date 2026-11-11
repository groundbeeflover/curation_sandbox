# SPDX-License-Identifier: Apache-2.0
# Tests that belong to no single module. Each module's own tests live in libs/<m>/tests/ and are
# added by cs_add_module; the integration-test executable arrives with M1-34.

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
