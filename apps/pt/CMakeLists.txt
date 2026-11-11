# SPDX-License-Identifier: Apache-2.0
# pt: the command-line path tracer. It renders a scene to an image file. cs-render replaces it
# in M1-17.
add_executable(pt main.cpp)
target_link_libraries(pt PRIVATE cs::core cs_options)
