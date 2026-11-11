# Throwaway prototype for M1-04 (issue #39). Not part of the main build; only ADR 0011 is merged.
#   cmake -S spike/qt-vulkan -B build/spike -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build build/spike && ./build/spike/qt-vulkan-spike
cmake_minimum_required(VERSION 3.25)
project(QtVulkanSpike LANGUAGES C CXX) # C: the generated Wayland protocol code

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Test)
find_package(Qt6 QUIET COMPONENTS WaylandClient GuiPrivate)
find_package(VulkanHeaders CONFIG QUIET)
if(NOT TARGET Vulkan::Headers)
    find_package(Vulkan REQUIRED) # the SDK on Windows; only its headers are used
endif()

add_library(spike_shell STATIC
    src/vk_renderer.cpp
    src/viewport.cpp
    src/main_window.cpp
    src/wayland_pointer.cpp)
target_include_directories(spike_shell PUBLIC src)
target_link_libraries(spike_shell PUBLIC Qt6::Widgets Vulkan::Headers ${CMAKE_DL_LIBS})

set(wayland_protocols_dir "/usr/share/wayland-protocols" CACHE PATH "wayland-protocols XML")
if(TARGET Qt6::WaylandClient AND TARGET Qt6::GuiPrivate AND EXISTS "${wayland_protocols_dir}/unstable/relative-pointer")
    qt6_generate_wayland_protocol_client_sources(spike_shell PRIVATE_CODE FILES
        "${wayland_protocols_dir}/unstable/relative-pointer/relative-pointer-unstable-v1.xml"
        "${wayland_protocols_dir}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml")
    target_link_libraries(spike_shell PUBLIC Qt6::WaylandClient Qt6::GuiPrivate)
    target_compile_definitions(spike_shell PRIVATE SPIKE_HAS_WAYLAND=1)
    message(STATUS "Wayland relative pointer: on")
else()
    target_compile_definitions(spike_shell PRIVATE SPIKE_HAS_WAYLAND=0)
    message(STATUS "Wayland relative pointer: off")
endif()

add_executable(qt-vulkan-spike src/main.cpp)
target_link_libraries(qt-vulkan-spike PRIVATE spike_shell)
if(WIN32)
    set_target_properties(qt-vulkan-spike PROPERTIES WIN32_EXECUTABLE ON)
endif()

enable_testing()
add_executable(tst_shell tests/tst_shell.cpp)
target_link_libraries(tst_shell PRIVATE spike_shell Qt6::Test)
add_test(NAME tst_shell COMMAND tst_shell)
set_tests_properties(tst_shell PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SPIKE_SCREENSHOT_DIR=${CMAKE_BINARY_DIR}/ui-out"
    LABELS ui)
