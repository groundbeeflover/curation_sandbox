# SPDX-License-Identifier: Apache-2.0
# NVIDIA FLIP, vendored unmodified (README.md says from where). FLIP.h is a single header that
# needs only the standard library. SYSTEM, so our warning flags do not apply to its code.
add_library(cs_third_party_flip INTERFACE)
target_include_directories(cs_third_party_flip SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
add_library(flip::flip ALIAS cs_third_party_flip)
