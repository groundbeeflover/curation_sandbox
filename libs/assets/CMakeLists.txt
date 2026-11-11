# SPDX-License-Identifier: Apache-2.0
find_package(TIFF REQUIRED)
find_package(lcms2 CONFIG REQUIRED)

# stb is header-only and its port defines no target, only the include directory.
find_package(Stb REQUIRED)
if(NOT TARGET Stb::Stb)
    add_library(Stb::Stb INTERFACE IMPORTED GLOBAL)
    set_target_properties(Stb::Stb PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${Stb_INCLUDE_DIR}")
endif()

# Image import: PNG and JPEG (stb_image), TIFF (libtiff), ICC profiles (Little CMS 2).
# KTX-Software arrives in M4.
cs_add_module(NAME assets DEPS core EXTERNAL Stb::Stb TIFF::TIFF lcms2::lcms2)
