# SPDX-License-Identifier: Apache-2.0
#
# cs_add_module(NAME <m> [DEPS <modules>...] [EXPORTS <modules>...] [EXTERNAL <targets>...])
#
# The whole of libs/<m>/CMakeLists.txt. Everything is found by glob, so adding a file, or a
# subdirectory such as src/commands/, never edits a CMake list:
#
#   libs/<m>/src/**/*.cpp       the library cs::<m> (target cs_<m>); src/ is also a private
#                               include directory, for headers only the module uses
#   libs/<m>/include/cs/<m>/    its public headers, included as <cs/<m>/...>
#   libs/<m>/tests/**/*.cpp     the Catch2 executable cs_<m>_tests; tests/main.cpp, when it
#                               exists, replaces Catch2's main
#   libs/<m>/fuzz/*.cpp         with CS_FUZZ, one libFuzzer executable cs_<m>_fuzz_<file> each
#
# DEPS are other modules, linked PRIVATE: a module sees the headers of the modules it declares
# and of no others, not even its dependencies' dependencies. That is how the build enforces the
# module graph of blueprint section 7 (the engine reads the world, never the document): an
# include of an undeclared module does not compile.
#
# EXPORTS are the DEPS whose types appear in the module's public headers (world's meshes are
# geom's). They are linked PUBLIC, so the module's users see their headers too, but not those
# modules' own private dependencies. Each must also be in DEPS. Everything else stays private:
# a public header may use only exported modules and modules that its users declare themselves.
#
# EXTERNAL are third-party targets (glm::glm, Qt6::Widgets), linked PUBLIC, because a module's
# headers may expose their types.
#
# A module with no sources yet becomes a header-only INTERFACE target, so the modules that
# declare it still configure and link; it compiles nothing.

# Module tests use Catch2 (from vcpkg.json) and register with CTest at the top level.
if(CS_BUILD_TESTS)
    enable_testing()
    find_package(Catch2 3 CONFIG REQUIRED)
    include(Catch)
endif()

# cs_add_subdirectories(<dir>): add every <dir>/*/ that has a CMakeLists.txt, in name order, so
# a new module or app needs no edit to the file that adds it.
function(cs_add_subdirectories parent)
    file(GLOB lists CONFIGURE_DEPENDS "${parent}/*/CMakeLists.txt")
    foreach(list IN LISTS lists)
        get_filename_component(dir "${list}" DIRECTORY)
        add_subdirectory("${dir}")
    endforeach()
endfunction()

function(cs_add_module)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "NAME" "DEPS;EXPORTS;EXTERNAL")
    if(NOT arg_NAME)
        message(FATAL_ERROR "cs_add_module: NAME is required")
    endif()
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "cs_add_module(${arg_NAME}): unknown arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()

    set(name ${arg_NAME})
    set(target cs_${name})
    set(dir ${CMAKE_CURRENT_SOURCE_DIR})
    foreach(export IN LISTS arg_EXPORTS)
        if(NOT export IN_LIST arg_DEPS)
            message(FATAL_ERROR "cs_add_module(${name}): EXPORTS ${export}, which is not in DEPS; "
                "a module re-exports only modules it declares")
        endif()
    endforeach()
    set(private_deps ${arg_DEPS})
    if(arg_EXPORTS)
        list(REMOVE_ITEM private_deps ${arg_EXPORTS})
    endif()
    list(TRANSFORM arg_DEPS PREPEND "cs::" OUTPUT_VARIABLE deps)
    list(TRANSFORM arg_EXPORTS PREPEND "cs::" OUTPUT_VARIABLE exports)
    list(TRANSFORM private_deps PREPEND "cs::")

    # Every test in this directory (unit tests and fuzz smoke runs) gets the module's name as a
    # CTest label, next to its Catch2 tags: `ctest -L <m>`.
    set_property(DIRECTORY PROPERTY LABELS ${name})

    file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${dir}/src/*.cpp")
    if(sources)
        add_library(${target} ${sources})
        target_include_directories(${target} PUBLIC "${dir}/include" PRIVATE "${dir}/src")
        target_link_libraries(${target}
            PUBLIC ${arg_EXTERNAL} ${exports}
            PRIVATE cs_options ${private_deps})
    else()
        # Private DEPS are link-only, as a static library passes on its PRIVATE dependencies:
        # consumers link them but do not see their headers. EXPORTS are passed on whole.
        list(TRANSFORM private_deps REPLACE "^(.+)$" "$<LINK_ONLY:\\1>"
            OUTPUT_VARIABLE link_only_deps)
        add_library(${target} INTERFACE)
        target_include_directories(${target} INTERFACE "${dir}/include")
        target_link_libraries(${target} INTERFACE ${arg_EXTERNAL} ${exports} ${link_only_deps})
    endif()
    add_library(cs::${name} ALIAS ${target})

    # Tests and fuzzers see the same modules as the library they test.
    set(links cs::${name} ${deps} cs_options)

    if(CS_BUILD_TESTS)
        file(GLOB_RECURSE tests CONFIGURE_DEPENDS "${dir}/tests/*.cpp")
        if(tests)
            set(test_target cs_${name}_tests)
            add_executable(${test_target} ${tests})
            if(EXISTS "${dir}/tests/main.cpp")
                set(catch Catch2::Catch2)
            else()
                set(catch Catch2::Catch2WithMain)
            endif()
            target_link_libraries(${test_target} PRIVATE ${links} ${catch})
            # The repository root, to find tests/scenes/ and data/.
            target_compile_definitions(${test_target} PRIVATE CS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
            # PRE_TEST: tests are listed when ctest runs, with ctest's environment (sanitizer
            # options), not at build time. catch_discover_tests also sets SKIP_RETURN_CODE 4,
            # Catch2's exit code for a skipped test, so SKIP() shows as skipped, not failed.
            catch_discover_tests(${test_target}
                TEST_PREFIX "${name}: "
                ADD_TAGS_AS_LABELS
                DISCOVERY_MODE PRE_TEST)
        endif()
    endif()

    if(CS_FUZZ)
        file(GLOB fuzzers CONFIGURE_DEPENDS "${dir}/fuzz/*.cpp")
        foreach(fuzzer IN LISTS fuzzers)
            get_filename_component(stem "${fuzzer}" NAME_WLE)
            set(fuzz_target cs_${name}_fuzz_${stem})
            add_executable(${fuzz_target} "${fuzzer}")
            target_link_libraries(${fuzz_target} PRIVATE ${links})
            target_compile_options(${fuzz_target} PRIVATE -fsanitize=fuzzer)
            target_link_options(${fuzz_target} PRIVATE -fsanitize=fuzzer)
            if(CS_BUILD_TESTS)
                # A short, seeded smoke run; real fuzzing runs the executable directly.
                add_test(NAME "${name}: fuzz ${stem}" COMMAND ${fuzz_target} -runs=1000 -seed=1)
                set_tests_properties("${name}: fuzz ${stem}" PROPERTIES LABELS fuzz)
            endif()
        endforeach()
    endif()
endfunction()
