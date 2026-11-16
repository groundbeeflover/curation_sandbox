# SPDX-License-Identifier: Apache-2.0
# cs-render: renders a project folder headless and writes its lux report (blueprint section 7).
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(cs-render main.cpp)
# world's public headers name light and material types, so its users declare both.
target_link_libraries(cs-render PRIVATE cs::core cs::doc cs::world cs::light cs::material
    cs::assets cs::engine nlohmann_json::nlohmann_json cs_options)
