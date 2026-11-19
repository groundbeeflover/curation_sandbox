# SPDX-License-Identifier: Apache-2.0
# The Qt shell is configured only with CS_BUILD_APP (the app preset), since it needs Qt 6, which
# cmake/CsQt.cmake finds.
if(NOT CS_BUILD_APP)
    return()
endif()

# moc, for the QObject classes of the library and its tests.
set(CMAKE_AUTOMOC ON)
cs_add_module(NAME ui DEPS editor gpu assets core EXTERNAL Qt6::Widgets)
# Walking's pointer lock on Wayland (src/viewport/wayland_pointer.cpp).
cs_qt_wayland_protocols(cs_ui)

if(TARGET cs_ui_tests)
    # The tests also reach the module's own headers in src/ (the viewport, the features).
    target_include_directories(cs_ui_tests PRIVATE src)
    target_link_libraries(cs_ui_tests PRIVATE Qt6::Test)
endif()
