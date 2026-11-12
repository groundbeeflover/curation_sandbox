# SPDX-License-Identifier: Apache-2.0
# World's public headers carry geom's meshes, so its users see geom (M1-35).
cs_add_module(NAME world DEPS core doc geom light material EXPORTS geom)
