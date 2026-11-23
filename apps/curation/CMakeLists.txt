# SPDX-License-Identifier: Apache-2.0
# curation: the desktop application (blueprint section 7), the Qt shell of libs/ui. Built only with
# CS_BUILD_APP (the app preset), which finds Qt (cmake/CsQt.cmake).
if(NOT CS_BUILD_APP)
    return()
endif()

file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
add_executable(curation ${sources})
target_link_libraries(curation PRIVATE cs::ui Qt6::Widgets cs_options)
