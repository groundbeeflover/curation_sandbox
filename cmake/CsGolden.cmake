# SPDX-License-Identifier: Apache-2.0
#
# cs_add_golden_tests()
#
# The golden-image tests of tests/golden/CMakeLists.txt: every tests/golden/<name>/test.json is one
# (tests/golden/README.md), found by glob, so adding a test is adding a directory. Each becomes the
# CTest test golden.<name>, which runs `cs-render golden tests/golden/<name>`, with the label
# golden, and gpu too when its tier is not path (the path tier is the engine's, on the CPU):
# `ctest -L golden` runs them all. A test with no reference for the running driver exits with 77,
# which CTest shows as skipped, not passed (cs_render::exit_skip, apps/cs-render/golden.hpp). A
# failed test writes its images to build/<preset>/golden-out/<name>/.

function(cs_add_golden_tests)
    if(NOT CS_BUILD_TESTS)
        return()
    endif()
    file(GLOB tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*/test.json")
    foreach(test IN LISTS tests)
        get_filename_component(dir "${test}" DIRECTORY)
        get_filename_component(name "${dir}" NAME)
        if(NOT name MATCHES "^[a-z0-9]+(-[a-z0-9]+)*$")
            message(FATAL_ERROR "${dir}: a golden test's directory is named in kebab-case, "
                "such as skeleton-path")
        endif()
        # The tier decides the labels, so a change to test.json configures again.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${test}")
        file(READ "${test}" json)
        string(JSON tier ERROR_VARIABLE error GET "${json}" tier)
        if(error)
            message(FATAL_ERROR "${test}: no tier: ${error}")
        endif()
        set(labels golden)
        if(NOT tier STREQUAL "path")
            list(APPEND labels gpu)
        endif()

        add_test(NAME golden.${name} COMMAND cs-render golden "${dir}")
        # A debug build with sanitizers renders about 60 times slower than release: skeleton-path
        # takes about 20 s on a 4-core CI runner. The timeout only catches a test that hangs.
        set_tests_properties(golden.${name} PROPERTIES
            LABELS "${labels}"
            SKIP_RETURN_CODE 77
            TIMEOUT 300)
    endforeach()
endfunction()
