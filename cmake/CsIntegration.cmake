# SPDX-License-Identifier: Apache-2.0
#
# cs_add_integration_tests()
#
# The whole of tests/integration/CMakeLists.txt: every tests/integration/**/*.cpp becomes the
# Catch2 executable cs_integration_tests, with the CTest label `integration` next to its Catch2
# tags (`ctest -L integration`).
#
# It is the one test executable that links every module, for checks that compare modules: a GPU
# tier against the engine, an editor camera rendered by the GPU, a compiled project under the lux
# meter. Such a test cannot live in a module's own tests, which see only the modules it declares,
# and no module may declare another just to be tested against it (blueprint section 7). Every
# module configured in this build is linked: ui only with CS_BUILD_APP.

function(cs_add_integration_tests)
    if(NOT CS_BUILD_TESTS)
        return()
    endif()
    file(GLOB_RECURSE tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
    if(NOT tests)
        return()
    endif()

    file(GLOB module_lists CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/libs/*/CMakeLists.txt")
    set(modules "")
    foreach(list IN LISTS module_lists)
        get_filename_component(dir "${list}" DIRECTORY)
        get_filename_component(name "${dir}" NAME)
        if(TARGET cs::${name})
            list(APPEND modules cs::${name})
        endif()
    endforeach()

    add_executable(cs_integration_tests ${tests})
    target_link_libraries(cs_integration_tests PRIVATE ${modules} cs_options Catch2::Catch2WithMain)
    # The repository root, to find tests/scenes/ and data/.
    target_compile_definitions(cs_integration_tests PRIVATE CS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    set_property(DIRECTORY PROPERTY LABELS integration)
    # PRE_TEST and SKIP() as in cs_add_module (cmake/CsModule.cmake).
    catch_discover_tests(cs_integration_tests
        TEST_PREFIX "integration: "
        ADD_TAGS_AS_LABELS
        DISCOVERY_MODE PRE_TEST)
endfunction()
