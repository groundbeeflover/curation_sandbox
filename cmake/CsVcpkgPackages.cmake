# SPDX-License-Identifier: Apache-2.0
#
# Every library of vcpkg.json is linked from vcpkg. Once every directory is configured, each
# imported target that the project's own targets link, directly or through other imported
# targets, must resolve to libraries and include directories under VCPKG_INSTALLED_DIR
# (build/<preset>/vcpkg_installed); otherwise the configure stops, naming the target and the file.
#
# Why: a find module may take the system's CMake package over vcpkg's files. CMake's FindTIFF
# tries libtiff's own config (Tiff) first and vcpkg's tiff port installs none, so on a machine
# with the system's libtiff config it linked /usr/lib/libtiff.so (M1-41). That build links a
# library of another version and licence than docs/dependencies.md records, on that machine only,
# and scripts/check-dependencies.py --installed cannot notice, as it reads vcpkg_installed.
#
# Not checked: Qt, which comes from outside vcpkg on purpose (docs/dependencies.md, "Outside
# vcpkg"), with everything its targets link; libraries linked by name alone (m, dl), which are
# the system's; and imported targets a directory creates itself with add_library(... IMPORTED),
# which are visible only there (the summary line names them).
#
# Imported targets are visible only in the directory that found them, so that the check, which
# runs at the end of the top-level directory, can read them, every package found after this file
# is included makes its imported targets global, as find_package(... GLOBAL) does. The cost: a
# module could link an external target that only another module finds. Each module still finds
# what it links.

if(NOT DEFINED VCPKG_INSTALLED_DIR)
    # Not configured with vcpkg's toolchain: there is nothing to check against.
    return()
endif()

set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ON)

# Imported targets from outside vcpkg on purpose, as a regular expression: Qt (Qt6:: and the
# versionless Qt::). A new one needs its row in docs/dependencies.md first.
set(cs_outside_vcpkg_targets "^Qt6?::")

# cs_vcpkg_items(<out> <target> <property>...): the entries of the properties of <target>, with
# generator expressions opened, so that every branch counts: $<LINK_ONLY:ZLIB::ZLIB> gives
# ZLIB::ZLIB, and $<$<CONFIG:Debug>:/x/libz.a> gives /x/libz.a in a release configure too. A
# colon separates a condition from its value, except in a namespace (ZLIB::ZLIB) and after a
# drive letter (C:/x). The pieces left over from the expressions (LINK_ONLY, CONFIG, Debug) name
# neither a target nor an absolute path, so the caller skips them.
function(cs_vcpkg_items out target)
    set(items "")
    foreach(property IN LISTS ARGN)
        get_target_property(value ${target} ${property})
        if(value)
            list(APPEND items ${value})
        endif()
    endforeach()
    string(REPLACE "::" "@cs_namespace@" items "${items}")
    string(REPLACE ":/" "@cs_drive@/" items "${items}")
    string(REGEX REPLACE "\\$<|>|,|:" ";" items "${items}")
    string(REPLACE "@cs_namespace@" "::" items "${items}")
    string(REPLACE "@cs_drive@" ":" items "${items}")
    list(REMOVE_ITEM items "")
    set(${out} "${items}" PARENT_SCOPE)
endfunction()

function(cs_check_vcpkg_packages)
    # Every target the project builds, and every imported target, in every directory.
    set(built "")
    set(imported "")
    set(directories "${CMAKE_CURRENT_SOURCE_DIR}")
    while(directories)
        list(POP_FRONT directories directory)
        get_directory_property(targets DIRECTORY "${directory}" BUILDSYSTEM_TARGETS)
        get_directory_property(imports DIRECTORY "${directory}" IMPORTED_TARGETS)
        get_directory_property(subdirectories DIRECTORY "${directory}" SUBDIRECTORIES)
        list(APPEND built ${targets})
        list(APPEND imported ${imports})
        list(APPEND directories ${subdirectories})
    endwhile()

    # The imported targets linked by built targets, then by those imported targets in turn. Each
    # remembers how it was reached, for the message: "cs_assets > TIFF::TIFF > TIFF::tiff". An
    # imported target that is not visible here cannot be read; the summary names it.
    set(queue "")
    set(not_visible "")
    foreach(target IN LISTS built)
        cs_vcpkg_items(items ${target} LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        foreach(item IN LISTS items)
            if(TARGET "${item}")
                if(NOT DEFINED "cs_via_${item}")
                    set("cs_via_${item}" "${target} > ${item}")
                    list(APPEND queue "${item}")
                endif()
            elseif(item IN_LIST imported)
                list(APPEND not_visible "${item}")
            endif()
        endforeach()
    endforeach()

    set(checked 0)
    set(problems "")
    while(queue)
        list(POP_FRONT queue name)
        set(via "${cs_via_${name}}")
        set(target "${name}")
        get_target_property(aliased ${target} ALIASED_TARGET)
        if(aliased)
            set(target "${aliased}")
        endif()
        get_target_property(is_imported ${target} IMPORTED)
        if(NOT is_imported OR target MATCHES "${cs_outside_vcpkg_targets}")
            continue()
        endif()
        math(EXPR checked "${checked} + 1")

        set(libraries IMPORTED_LOCATION IMPORTED_IMPLIB)
        set(links INTERFACE_LINK_LIBRARIES IMPORTED_LINK_INTERFACE_LIBRARIES
            IMPORTED_LINK_DEPENDENT_LIBRARIES)
        get_target_property(configurations ${target} IMPORTED_CONFIGURATIONS)
        if(configurations)
            string(TOUPPER "${configurations}" configurations)
            foreach(configuration IN LISTS configurations)
                list(APPEND libraries IMPORTED_LOCATION_${configuration}
                    IMPORTED_IMPLIB_${configuration})
                list(APPEND links IMPORTED_LINK_INTERFACE_LIBRARIES_${configuration}
                    IMPORTED_LINK_DEPENDENT_LIBRARIES_${configuration})
            endforeach()
        endif()
        cs_vcpkg_items(library_files ${target} ${libraries})
        cs_vcpkg_items(include_dirs ${target}
            INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
        cs_vcpkg_items(link_dirs ${target} INTERFACE_LINK_DIRECTORIES)
        cs_vcpkg_items(link_items ${target} ${links})

        # Absolute paths among the link items are libraries too; targets are followed.
        foreach(item IN LISTS link_items)
            if(TARGET "${item}")
                if(NOT DEFINED "cs_via_${item}")
                    set("cs_via_${item}" "${via} > ${item}")
                    list(APPEND queue "${item}")
                endif()
            elseif(IS_ABSOLUTE "${item}")
                list(APPEND library_files "${item}")
            elseif(item IN_LIST imported)
                list(APPEND not_visible "${item}")
            endif()
        endforeach()

        set(outside "")
        foreach(kind IN ITEMS library_files include_dirs link_dirs)
            list(REMOVE_DUPLICATES ${kind})
            foreach(path IN LISTS ${kind})
                if(NOT IS_ABSOLUTE "${path}")
                    continue()
                endif()
                cmake_path(IS_PREFIX VCPKG_INSTALLED_DIR "${path}" NORMALIZE inside)
                if(NOT inside)
                    if(kind STREQUAL "library_files")
                        string(APPEND outside "\n      library ${path}")
                    elseif(kind STREQUAL "include_dirs")
                        string(APPEND outside "\n      include directory ${path}")
                    else()
                        string(APPEND outside "\n      link directory ${path}")
                    endif()
                endif()
            endforeach()
        endforeach()
        if(outside)
            string(APPEND problems "\n    ${target}, linked as ${via}:${outside}")
        endif()
    endwhile()

    list(REMOVE_DUPLICATES not_visible)

    file(RELATIVE_PATH installed "${CMAKE_SOURCE_DIR}" "${VCPKG_INSTALLED_DIR}")
    if(problems)
        message(FATAL_ERROR "Libraries from vcpkg.json resolve to files outside ${installed}, so "
            "this machine would link another build of them than the one vcpkg.json pins and "
            "docs/dependencies.md records:${problems}\n"
            "A find module probably found the system's CMake package before vcpkg's files: "
            "disable that package before the module's find_package, as "
            "libs/assets/CMakeLists.txt does for libtiff's (CMAKE_DISABLE_FIND_PACKAGE_Tiff), and "
            "configure again. A target that comes from outside vcpkg on purpose needs a row under "
            "\"Outside vcpkg\" in docs/dependencies.md, then a place in cs_outside_vcpkg_targets "
            "in cmake/CsVcpkgPackages.cmake.")
    endif()
    set(summary "${checked} imported targets linked from ${installed}")
    if(not_visible)
        list(JOIN not_visible ", " names)
        string(APPEND summary " (not checked, imported by their directory alone: ${names})")
    endif()
    message(STATUS "vcpkg packages: ${summary}")
endfunction()

cmake_language(DEFER CALL cs_check_vcpkg_packages)
