# SPDX-License-Identifier: Apache-2.0
# The Qt shell is configured only with CS_BUILD_APP (the app preset), since it needs Qt 6, which
# cmake/CsQt.cmake finds.
if(NOT CS_BUILD_APP)
    return()
endif()

# moc, for the QObject classes of the library and its tests.
set(CMAKE_AUTOMOC ON)
# world, doc and assets come through editor's and gpu's re-exports (blueprint section 7). The main
# window's header uses doc's History, Transaction and ChangeList, so ui re-exports editor, which
# re-exports doc. The engine is private: the lux panel (src/features/lux_panel.cpp) reads the lux
# meter, and no public header of ui names an engine type.
cs_add_module(NAME ui DEPS editor gpu assets core engine EXPORTS editor EXTERNAL Qt6::Widgets)
# Walking's pointer lock on Wayland (src/viewport/wayland_pointer.cpp).
cs_qt_wayland_protocols(cs_ui)
# moc reads the QObject classes of the public headers too, which the module's sources (src/) do
# not list.
file(GLOB_RECURSE ui_public_headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp")
target_sources(cs_ui PRIVATE ${ui_public_headers})
# Every src/features/*.cpp registers itself: register_features(MainWindow&) calls them all.
cs_generate_registry(ui features PARAMETERS "MainWindow&" INCLUDES "features/feature.hpp")
# Works > Add sample painting (src/features/sample_paintings.cpp) lists the manifests of
# data/samples/paintings/ and looks for their images in build/samples/paintings/, where
# scripts/fetch-samples.py puts them. Packaging them with the app is M4's.
target_compile_definitions(cs_ui PRIVATE
    CS_SAMPLE_MANIFESTS_DIR="${CMAKE_SOURCE_DIR}/data/samples/paintings"
    CS_SAMPLE_IMAGES_DIR="${CMAKE_SOURCE_DIR}/build/samples/paintings")

if(TARGET cs_ui_tests)
    # The tests also reach the module's own headers in src/ (the viewport, the features).
    target_include_directories(cs_ui_tests PRIVATE src)
    target_link_libraries(cs_ui_tests PRIVATE Qt6::Test)
    # Where UI tests write their screenshots: build/<preset>/ui-out/.
    target_compile_definitions(cs_ui_tests PRIVATE CS_UI_OUT_DIR="${CMAKE_BINARY_DIR}/ui-out")
endif()
