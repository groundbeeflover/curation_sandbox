# SPDX-License-Identifier: Apache-2.0
# cs-render: renders a project folder headless and writes its lux report (blueprint section 7), and
# runs golden-image tests (`cs-render golden`, tests/golden/README.md).
find_package(nlohmann_json CONFIG REQUIRED)

file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
add_executable(cs-render ${sources})
# world's public headers name light and material types, so its users declare both.
target_link_libraries(cs-render PRIVATE cs::core cs::doc cs::world cs::light cs::material
    cs::assets cs::engine nlohmann_json::nlohmann_json cs_options)
# Where `cs-render golden` writes a failed test's images unless told otherwise:
# build/<preset>/golden-out/<test>/, the place CTest's output and CI's artefact point at.
target_compile_definitions(cs-render PRIVATE CS_GOLDEN_OUT_DIR="${CMAKE_BINARY_DIR}/golden-out")
