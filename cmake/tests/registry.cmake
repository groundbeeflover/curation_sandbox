# SPDX-License-Identifier: Apache-2.0
#
# The cmake_registry test (tests/CMakeLists.txt): builds the fixture project in registry/ with
# the repository's cs_add_module and cs_generate_registry, and checks, by building it and running
# list_greetings, which prints what the generated register_greetings registers, that
#
#   - the generated function calls register_<name> for every src/greetings/<name>.cpp, in file
#     name order, passing on every parameter;
#   - adding a file adds a call, and removing one removes its call, with `cmake --build` alone
#     and no CMake edit;
#   - files other than .cpp in the directory are left alone;
#   - a file whose name (less .cpp) is not a C++ identifier stops the build with a message
#     naming it;
#   - a registry with no files compiles, without warnings, to a function that does nothing.
#
# cmake -D CS_REPO_DIR=... -D WORK_DIR=... -D GENERATOR=... -D CXX_COMPILER=... -D BUILD_TYPE=...
#       -P registry.cmake

foreach(var CS_REPO_DIR WORK_DIR GENERATOR CXX_COMPILER BUILD_TYPE)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "registry.cmake: -D ${var}=... is required")
    endif()
endforeach()

set(src "${WORK_DIR}/src")
set(bin "${WORK_DIR}/build")
set(greetings "${src}/libs/greet/src/greetings")
# BUILD_TYPE is the parent's configuration, so multi-config generators work too.
set(build ${CMAKE_COMMAND} --build "${bin}" --config "${BUILD_TYPE}")

function(run what expect)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
    string(REPLACE "\r" "" out "${out}")
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

# Builds the fixture, with no CMake edit, runs list_greetings and checks what it prints.
function(expect_greetings what printed)
    run("building after ${what}" succeeds ${build})
    file(GLOB_RECURSE exe LIST_DIRECTORIES false
        "${bin}/list_greetings" "${bin}/list_greetings.exe")
    run("running list_greetings after ${what}" succeeds ${exe})
    if(NOT output STREQUAL printed)
        message(FATAL_ERROR "FAIL: after ${what}, list_greetings printed\n${output}\n"
            "instead of\n${printed}")
    endif()
    message(STATUS "ok: after ${what}, the registry calls what the directory holds")
    set(output "${output}" PARENT_SCOPE)
endfunction()

function(write_greeting name)
    file(WRITE "${greetings}/${name}.cpp"
        "// SPDX-License-Identifier: Apache-2.0\n#include <string>\n#include <vector>\n"
        "namespace cs::greet {\n"
        "void register_${name}(std::vector<std::string>& names, const std::string& prefix) {\n"
        "    names.push_back(prefix + \"${name}\");\n}\n}\n")
endfunction()

# ---- A fresh copy, configured and built --------------------------------------
# Warnings are errors, so the generated file must compile cleanly, also when it calls nothing.
file(REMOVE_RECURSE "${WORK_DIR}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/registry/" DESTINATION "${src}")

run("configuring the fixture" succeeds ${CMAKE_COMMAND} -S "${src}" -B "${bin}" -G "${GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" "-DCMAKE_CXX_COMPILER_LAUNCHER=${CXX_COMPILER_LAUNCHER}"
    "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}" "-DCS_BUILD_TESTS=OFF" "-DCS_WARNINGS_AS_ERRORS=ON"
    "-DCS_REPO_DIR=${CS_REPO_DIR}")
expect_greetings("configuring" "> bye\n> hello\n2 greetings\n")

file(GLOB_RECURSE generated LIST_DIRECTORIES false "${bin}/register_greetings.cpp")
file(READ "${generated}" output)
expect_output("the generated file says where it comes from" "cs_generate_registry[^\n]*\n[^\n]*greetings")
expect_output("the generated file declares what it calls"
    "void register_bye\\(std::vector<std::string>& [a-z0-9_]+, const std::string& [a-z0-9_]+\\);")

# ---- Adding and removing files -----------------------------------------------
write_greeting(welcome)
expect_greetings("adding welcome.cpp" "> bye\n> hello\n> welcome\n3 greetings\n")

file(REMOVE "${greetings}/hello.cpp")
expect_greetings("removing hello.cpp" "> bye\n> welcome\n2 greetings\n")

# A header or a note beside the registered files is not a registered file.
file(WRITE "${greetings}/shared.hpp" "#pragma once\n")
file(WRITE "${greetings}/README.md" "Greetings.\n")
expect_greetings("adding shared.hpp and README.md" "> bye\n> welcome\n2 greetings\n")

# ---- A file name that cannot name a function ---------------------------------
file(WRITE "${greetings}/good-bye.cpp" "// SPDX-License-Identifier: Apache-2.0\n")
run("building with good-bye.cpp, which cannot name a function" fails ${build})
# CMake wraps the message, so the file's name may be lines after the call's.
expect_output("the message names the file" "cs_generate_registry\\(greet greetings\\):.*good-bye\\.cpp")
file(REMOVE "${greetings}/good-bye.cpp")
# Not read as good.cpp, which would register a function no file defines.
file(WRITE "${greetings}/good.bye.cpp" "// SPDX-License-Identifier: Apache-2.0\n")
run("building with good.bye.cpp, which cannot name a function" fails ${build})
expect_output("the message names the file" "cs_generate_registry\\(greet greetings\\):.*good\\.bye\\.cpp")
file(REMOVE "${greetings}/good.bye.cpp")
expect_greetings("removing the badly named files" "> bye\n> welcome\n2 greetings\n")

# ---- An empty registry ---------------------------------------------------------
file(REMOVE "${greetings}/bye.cpp" "${greetings}/welcome.cpp")
expect_greetings("removing every greeting" "0 greetings\n")
