# SPDX-License-Identifier: Apache-2.0
#
# Qt 6 for the ui module and the app, only with CS_BUILD_APP (ADR 0006, ADR 0011). Qt does not
# come from vcpkg: CI installs the pinned version with aqtinstall, and developer machines use the
# distribution's Qt or Qt's installer at that version or newer (docs/dependencies.md).
#
# On Linux, walking locks the pointer through two Wayland protocols that Qt has no API for (ADR
# 0011, point 6), so this also finds Qt's Wayland client, Qt's private Gui headers for the one
# call that needs them, and the protocols' XML; a missing one stops the configure.
# cs_qt_wayland_protocols(<target>) then generates the protocols' client code into <target>.

if(NOT CS_BUILD_APP)
    return()
endif()

# The pin of docs/dependencies.md: change both together.
set(CS_QT_MIN_VERSION 6.10.3)
find_package(Qt6 ${CS_QT_MIN_VERSION} COMPONENTS Core Gui Widgets Test)
if(NOT Qt6_FOUND)
    message(FATAL_ERROR "Qt ${CS_QT_MIN_VERSION} or newer (Core, Gui, Widgets, Test) was not "
        "found; CS_BUILD_APP needs it. Install it (qt6-base on Arch, qt6-base-dev on Debian and "
        "Ubuntu, qt6-qtbase-devel on Fedora, Qt's installer on Windows), or point "
        "CMAKE_PREFIX_PATH at it.")
endif()

if(LINUX)
    # The generated protocol code is partly C.
    enable_language(C)
    # Qt warns that private modules tie the build to one Qt version. The app always ships with
    # the Qt it was built with (ADR 0011, point 1), and the one private call is in one file.
    set(QT_NO_PRIVATE_MODULE_WARNING ON)
    find_package(Qt6 ${CS_QT_MIN_VERSION} QUIET COMPONENTS WaylandClient GuiPrivate)
    if(NOT TARGET Qt6::WaylandClient)
        message(FATAL_ERROR "Qt's Wayland client (Qt6::WaylandClient) was not found; walking "
            "needs it on Linux. Install libwayland-dev and qt6-wayland-dev on Debian and Ubuntu, "
            "qt6-wayland on Arch, qt6-qtwayland-devel on Fedora. With Qt from aqtinstall the "
            "module is there, but its CMake package also needs libwayland-dev.")
    endif()
    if(NOT TARGET Qt6::GuiPrivate)
        message(FATAL_ERROR "Qt's private Gui headers (Qt6::GuiPrivate) were not found; walking "
            "needs them on Linux. Install qt6-base-private-dev on Debian and Ubuntu, "
            "qt6-qtbase-private-devel on Fedora; on Arch, qt6-base has them.")
    endif()

    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_get_variable(cs_wayland_protocols_hint wayland-protocols pkgdatadir)
    endif()
    find_path(CS_WAYLAND_PROTOCOLS_DIR
        NAMES unstable/relative-pointer/relative-pointer-unstable-v1.xml
        HINTS "${cs_wayland_protocols_hint}"
        PATHS /usr/share/wayland-protocols /usr/local/share/wayland-protocols
        DOC "The wayland-protocols directory, with unstable/relative-pointer/ in it")
    set(CS_WAYLAND_PROTOCOLS
        "${CS_WAYLAND_PROTOCOLS_DIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml"
        "${CS_WAYLAND_PROTOCOLS_DIR}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml")
    foreach(protocol IN LISTS CS_WAYLAND_PROTOCOLS)
        if(NOT EXISTS "${protocol}")
            message(FATAL_ERROR "The Wayland protocol ${protocol} was not found; walking needs "
                "it on Linux. Install wayland-protocols (wayland-protocols-devel on Fedora), or "
                "set CS_WAYLAND_PROTOCOLS_DIR to its share/wayland-protocols directory.")
        endif()
    endforeach()
endif()

# cs_qt_wayland_protocols(<target>): on Linux, generates the relative-pointer and
# pointer-constraints client code into <target>, links Qt's Wayland client and private Gui
# headers privately, and defines CS_WAYLAND=1. Elsewhere it defines CS_WAYLAND=0.
function(cs_qt_wayland_protocols target)
    if(NOT LINUX)
        target_compile_definitions(${target} PRIVATE CS_WAYLAND=0)
        return()
    endif()
    qt6_generate_wayland_protocol_client_sources(${target} PRIVATE_CODE
        FILES ${CS_WAYLAND_PROTOCOLS})
    target_link_libraries(${target} PRIVATE Qt6::WaylandClient Qt6::GuiPrivate)
    target_compile_definitions(${target} PRIVATE CS_WAYLAND=1)
endfunction()
