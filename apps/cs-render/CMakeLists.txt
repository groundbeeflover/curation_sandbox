# SPDX-License-Identifier: Apache-2.0
# cs-render: renders a project folder headless, with the path tracer or the Standard tier, and
# writes its lux report (blueprint section 7); runs golden-image tests (`cs-render golden`,
# tests/golden/README.md); and prints the Vulkan device report (`cs-render --device-report`).
find_package(nlohmann_json CONFIG REQUIRED)

file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
add_executable(cs-render ${sources})
# world's public headers name light and material types, so its users declare both; gpu's name
# world and assets types, which it does not export.
target_link_libraries(cs-render PRIVATE cs::core cs::doc cs::world cs::light cs::material
    cs::assets cs::engine cs::gpu nlohmann_json::nlohmann_json cs_options)
# Where `cs-render golden` writes a failed test's images unless told otherwise:
# build/<preset>/golden-out/<test>/, the place CTest's output and CI's artefact point at.
target_compile_definitions(cs-render PRIVATE CS_GOLDEN_OUT_DIR="${CMAKE_BINARY_DIR}/golden-out")

# Checks of the GPU commands (checks.cmake): --device-report prints the device report, and
# --tier standard renders the skeleton. Without a Vulkan device they skip, unless
# CS_REQUIRE_GPU=1 is set, when they fail.
if(CS_BUILD_TESTS)
    function(cs_render_gpu_check check description)
        set(name "cs-render: ${description}")
        add_test(NAME "${name}"
            COMMAND ${CMAKE_COMMAND}
                -D "CS_RENDER=$<TARGET_FILE:cs-render>"
                -D "SCENE=${PROJECT_SOURCE_DIR}/tests/scenes/skeleton"
                -D "WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/check-${check}"
                -D "CONFIG=$<CONFIG>"
                -D "CHECK=${check}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/checks.cmake")
        set_tests_properties("${name}" PROPERTIES
            LABELS "cs-render;gpu"
            SKIP_REGULAR_EXPRESSION "cs-render check skipped: no Vulkan device"
            TIMEOUT 120)
    endfunction()
    cs_render_gpu_check(device-report "--device-report prints the Vulkan device report")
    cs_render_gpu_check(standard "--tier standard renders the skeleton through the Standard tier")
endif()
