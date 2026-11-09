cmake_minimum_required(VERSION 3.25)

project(CurationSandbox
    VERSION 0.1.0
    DESCRIPTION "A digital maquette for exhibitions, with physically accurate light"
    LANGUAGES CXX)

# ---- Options -----------------------------------------------------------------
option(CS_BUILD_TESTS "Build the unit tests" ON)
option(CS_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer (GCC and Clang)" OFF)
option(CS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

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

# ---- Shared compiler flags ---------------------------------------------------
# Every target links cs_options, so they all get the same warnings and sanitizers.
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
        # Sanitizers catch memory errors and undefined behaviour while the program runs.
        target_compile_options(cs_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(cs_options INTERFACE -fsanitize=address,undefined)
    endif()
endif()

# ---- Project parts -----------------------------------------------------------
add_subdirectory(core)
add_subdirectory(apps)

if(CS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
