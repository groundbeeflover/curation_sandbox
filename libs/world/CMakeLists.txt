# SPDX-License-Identifier: Apache-2.0
# World's public headers carry geom's meshes, light's luminaires and material's painting
# materials, so its users see those three (M1-35, M1-26). Never doc: the engine and the GPU read
# the world, not the document.
cs_add_module(NAME world DEPS core doc geom light material EXPORTS geom light material)

# A test that every cs/world/ header compiles for a user of world that declares only what the
# engine declares, and that doc is not reachable through world's exports (#61). The source is
# generated from tests/headers_without_doc.cpp.in with one include per header found.
if(CS_BUILD_TESTS)
    file(GLOB world_headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/cs/world/*.hpp")
    set(world_includes "")
    foreach(header IN LISTS world_headers)
        get_filename_component(header_name "${header}" NAME)
        string(APPEND world_includes "#include <cs/world/${header_name}>\n")
    endforeach()
    configure_file(tests/headers_without_doc.cpp.in headers_without_doc.cpp @ONLY)
    add_library(cs_world_headers_without_doc OBJECT
        "${CMAKE_CURRENT_BINARY_DIR}/headers_without_doc.cpp")
    target_link_libraries(cs_world_headers_without_doc
        PRIVATE cs::world cs::core cs::light cs::material cs_options)
    add_test(NAME "world: public headers compile without doc"
        COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                --target cs_world_headers_without_doc --config $<CONFIG>)
endif()
