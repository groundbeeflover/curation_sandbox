# SPDX-License-Identifier: Apache-2.0
# libtiff from vcpkg, through CMake's FindTIFF, as the port's usage note says. FindTIFF first
# tries libtiff's own CMake config (Tiff), which vcpkg's port does not install, so a system's
# config (/usr/lib/cmake/tiff) would win; without it, FindTIFF searches vcpkg's prefix first. A
# Tiff_DIR left in the cache by an earlier configure is dropped, as nothing reads it now.
set(CMAKE_DISABLE_FIND_PACKAGE_Tiff TRUE)
unset(Tiff_DIR CACHE)
find_package(TIFF REQUIRED)
find_package(lcms2 CONFIG REQUIRED)

# Image import: TIFF (libtiff) and ICC profiles (Little CMS 2); PNG and JPEG through stb_image,
# which comes with core. KTX-Software arrives in M4.
cs_add_module(NAME assets DEPS core EXTERNAL TIFF::TIFF lcms2::lcms2)
