# SPDX-License-Identifier: Apache-2.0
find_package(glm CONFIG REQUIRED)
find_package(tinyexr CONFIG REQUIRED)

# stb is header-only and its port defines no target, only the include directory. Linked here,
# it reaches every module, as glm does: assets uses stb_image, gpu later stb_truetype.
find_package(Stb REQUIRED)
add_library(Stb::Stb INTERFACE IMPORTED)
set_target_properties(Stb::Stb PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${Stb_INCLUDE_DIR}")

# NVIDIA FLIP, vendored in third_party/flip/: image comparison for golden images (M1-07).
add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/flip" "${PROJECT_BINARY_DIR}/third_party/flip")

# EXR (tinyexr) and PNG (stb_image_write) files, and image comparison (FLIP).
cs_add_module(NAME core EXTERNAL glm::glm unofficial::tinyexr::tinyexr Stb::Stb flip::flip)
