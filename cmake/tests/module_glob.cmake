# SPDX-License-Identifier: Apache-2.0
#
# The cmake_module_glob test (tests/CMakeLists.txt): builds the fixture project in module_glob/
# with the repository's cs_add_module and checks, by building and running it, that
#
#   - a module with no sources configures without error, and modules that declare it link;
#   - test executables carry the module's name and their Catch2 tags as CTest labels, see
#     CS_SOURCE_DIR, use the module's tests/main.cpp when there is one, and report Catch2's
#     exit code 4 as skipped;
#   - a new source file, in a new subdirectory of src/, and a new test file are built by
#     `cmake --build` alone, with no CMake edit;
#   - including a header of a module that is not declared fails to compile, also when it is a
#     dependency of a declared module;
#   - a module that re-exports a dependency (EXPORTS), compiled or header-only, lets its users
#     include that dependency's headers (delta and epsilon re-export beta; zeta and eta use
#     beta through them), but not the dependency's own private dependencies;
#   - exporting a module that is not in DEPS stops configuring with a message naming it.
#
# cmake -D CS_REPO_DIR=... -D WORK_DIR=... -D GENERATOR=... -D CXX_COMPILER=... -D BUILD_TYPE=...
#       -D Catch2_DIR=... -D CTEST_COMMAND=... -P module_glob.cmake

foreach(var CS_REPO_DIR WORK_DIR GENERATOR CXX_COMPILER BUILD_TYPE Catch2_DIR CTEST_COMMAND)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "module_glob.cmake: -D ${var}=... is required")
    endif()
endforeach()

set(src "${WORK_DIR}/src")
set(bin "${WORK_DIR}/build")
# BUILD_TYPE is the parent's configuration, so multi-config generators work too.
set(build ${CMAKE_COMMAND} --build "${bin}" --config "${BUILD_TYPE}")
set(ctest ${CTEST_COMMAND} --test-dir "${bin}" -C "${BUILD_TYPE}")

function(run what expect)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
    set(output "${out}" PARENT_SCOPE)
    if(expect STREQUAL "succeeds" AND NOT result EQUAL 0)
        message(FATAL_ERROR "FAIL: ${what} failed (${result}):\n${out}")
    elseif(expect STREQUAL "fails" AND result EQUAL 0)
        message(FATAL_ERROR "FAIL: ${what} succeeded, but should have failed:\n${out}")
    endif()
    message(STATUS "ok: ${what} ${expect}")
endfunction()

function(expect_output what regex)
    if(NOT output MATCHES "${regex}")
        message(FATAL_ERROR "FAIL: ${what}: no match for '${regex}' in:\n${output}")
    endif()
    message(STATUS "ok: ${what}")
endfunction()

# Lists the tests that carry exactly this label and checks how many there are.
function(expect_label_count label count)
    run("listing label ${label}" succeeds ${ctest} -N -L "^${label}$")
    expect_output("${count} tests labelled ${label}" "Total Tests: ${count}\n")
endfunction()

# ---- A fresh copy, configured and built --------------------------------------
file(REMOVE_RECURSE "${WORK_DIR}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/module_glob/" DESTINATION "${src}")

run("configuring the fixture" succeeds ${CMAKE_COMMAND} -S "${src}" -B "${bin}" -G "${GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" "-DCMAKE_CXX_COMPILER_LAUNCHER=${CXX_COMPILER_LAUNCHER}"
    "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
    "-DCatch2_DIR=${Catch2_DIR}" "-DCS_REPO_DIR=${CS_REPO_DIR}")
run("building the fixture" succeeds ${build})

# ---- Labels, CS_SOURCE_DIR, tests/main.cpp, skipped tests --------------------
run("running the fixture's tests" succeeds ${ctest} --no-tests=error
    --output-on-failure)
expect_output("the SKIP test is reported as skipped" "a skipped test is reported as skipped[ .]*\\*+Skipped")
expect_output("no test failed" "100% tests passed")
expect_label_count(alpha 2)
expect_label_count(alpha-tag 2)
expect_label_count(beta 3)
expect_label_count(beta-tag 3)

# ---- New files, no CMake edit ------------------------------------------------
# One at a time, so that each glob has to notice its own new file.
file(WRITE "${src}/libs/alpha/include/cs/alpha/extra.hpp"
    "#pragma once\nnamespace cs::alpha {\nint extra();\n}\n")
file(WRITE "${src}/libs/alpha/src/extra/extra.cpp"
    "#include <cs/alpha/extra.hpp>\nnamespace cs::alpha {\nint extra() { return 42; }\n}\n")
run("building after adding src/extra/extra.cpp" succeeds ${build})
file(WRITE "${src}/libs/alpha/tests/test_extra.cpp"
    "#include <cs/alpha/extra.hpp>\n#include <catch2/catch_test_macros.hpp>\n"
    "TEST_CASE(\"a new test of a new source\", \"[alpha-tag]\") { CHECK(cs::alpha::extra() == 42); }\n")
run("building after adding tests/test_extra.cpp" succeeds ${build})
expect_label_count(alpha 3)
run("running the new test" succeeds ${ctest} --no-tests=error
    --output-on-failure -R "a new test of a new source")

# ---- Undeclared modules do not compile ---------------------------------------
# alpha does not declare beta.
file(WRITE "${src}/libs/alpha/src/sneaky.cpp" "#include <cs/beta/beta.hpp>\n")
run("building alpha including a header of beta, which alpha does not declare" fails
    ${build})
expect_output("the build fails on that include" "cs/beta/beta\\.hpp")
file(REMOVE "${src}/libs/alpha/src/sneaky.cpp")

# gamma declares beta, which declares alpha; gamma does not declare alpha.
file(WRITE "${src}/libs/gamma/src/sneaky.cpp" "#include <cs/alpha/alpha.hpp>\n")
run("building gamma including a header of alpha, a dependency of its dependency" fails
    ${build})
expect_output("the build fails on that include" "cs/alpha/alpha\\.hpp")
file(REMOVE "${src}/libs/gamma/src/sneaky.cpp")

# ---- Re-exported dependencies -------------------------------------------------
# zeta and eta include beta's header, which they see only because delta (compiled) and epsilon
# (header-only) re-export beta; the fixture's build above already proved they compile. What
# beta keeps private stays hidden: alpha is a dependency of beta that beta does not export.
file(WRITE "${src}/libs/zeta/src/sneaky.cpp" "#include <cs/alpha/alpha.hpp>\n")
run("building zeta including alpha, private to beta, which delta re-exports"
    fails ${build})
expect_output("the build fails on that include" "cs/alpha/alpha\\.hpp")
file(REMOVE "${src}/libs/zeta/src/sneaky.cpp")

file(WRITE "${src}/libs/eta/src/sneaky.cpp" "#include <cs/alpha/alpha.hpp>\n")
run("building eta including alpha, private to beta, which epsilon re-exports"
    fails ${build})
expect_output("the build fails on that include" "cs/alpha/alpha\\.hpp")
file(REMOVE "${src}/libs/eta/src/sneaky.cpp")

# A module may re-export only what it declares.
file(WRITE "${src}/libs/theta/CMakeLists.txt" "cs_add_module(NAME theta DEPS alpha EXPORTS beta)\n")
run("configuring theta, which exports beta without declaring it" fails
    ${CMAKE_COMMAND} -S "${src}" -B "${bin}")
expect_output("the message names theta and beta" "cs_add_module\\(theta\\)[^\n]*beta")
file(REMOVE_RECURSE "${src}/libs/theta")
run("configuring again once theta is gone" succeeds ${CMAKE_COMMAND} -S "${src}" -B "${bin}")

run("building again once the includes are gone" succeeds ${build})
