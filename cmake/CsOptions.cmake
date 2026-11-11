# SPDX-License-Identifier: Apache-2.0
#
# Build options, language settings and the cs_options target that every target links, so they
# all get the same warnings and sanitizers. The root CMakeLists.txt includes this file.

option(CS_BUILD_TESTS "Build the unit tests" ON)
option(CS_BUILD_APP "Build the Qt application and the ui module (needs Qt 6)" OFF)
option(CS_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer (GCC and Clang)" OFF)
option(CS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(CS_FUZZ "Build every libs/<module>/fuzz/*.cpp as a libFuzzer target (Clang only)" OFF)

# ---- Language settings -------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Writes compile_commands.json, which editors (clangd, VS Code) use to understand the code.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to a debug build when nothing else is asked for.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

if(CS_FUZZ AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "CS_FUZZ needs Clang (libFuzzer); use the fuzz preset, which selects it.")
endif()

# ---- Shared compiler flags ---------------------------------------------------
add_library(cs_options INTERFACE)

if(MSVC)
    target_compile_options(cs_options INTERFACE /W4 /permissive- /utf-8)
    if(CS_WARNINGS_AS_ERRORS)
        target_compile_options(cs_options INTERFACE /WX)
    endif()
else()
    target_compile_options(cs_options INTERFACE -Wall -Wextra -Wpedantic)
    if(CS_WARNINGS_AS_ERRORS)
        target_compile_options(cs_options INTERFACE -Werror)
    endif()
    if(CS_SANITIZERS)
        # Sanitizers catch memory errors and undefined behaviour while the program runs. Undefined
        # behaviour aborts rather than printing and carrying on, so a test that hits it fails.
        target_compile_options(cs_options INTERFACE
            -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        target_link_options(cs_options INTERFACE -fsanitize=address,undefined)
    endif()
    if(CS_FUZZ)
        # Coverage instrumentation for libFuzzer in every target; the fuzz executables themselves
        # add -fsanitize=fuzzer, which brings libFuzzer's main (see CsModule.cmake).
        target_compile_options(cs_options INTERFACE -fsanitize=fuzzer-no-link)
        target_link_options(cs_options INTERFACE -fsanitize=fuzzer-no-link)
    endif()
endif()
