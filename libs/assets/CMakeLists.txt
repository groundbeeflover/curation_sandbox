# SPDX-License-Identifier: Apache-2.0
find_package(TIFF REQUIRED)
find_package(lcms2 CONFIG REQUIRED)

# Image import: TIFF (libtiff) and ICC profiles (Little CMS 2); PNG and JPEG through stb_image,
# which comes with core. KTX-Software arrives in M4.
cs_add_module(NAME assets DEPS core EXTERNAL TIFF::TIFF lcms2::lcms2)
