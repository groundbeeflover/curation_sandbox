# SPDX-License-Identifier: Apache-2.0
# Every apps/*/ with a CMakeLists.txt is added, so a task adds an app without editing this file.
file(GLOB cs_apps CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*/CMakeLists.txt")
foreach(app IN LISTS cs_apps)
    get_filename_component(app_dir "${app}" DIRECTORY)
    add_subdirectory("${app_dir}")
endforeach()
