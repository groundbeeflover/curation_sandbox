# SPDX-License-Identifier: Apache-2.0
find_package(embree 4 CONFIG REQUIRED)

# Embree 4 for CPU ray tracing. Open Image Denoise arrives in M3.
cs_add_module(NAME engine DEPS core world light material assets EXTERNAL embree)
