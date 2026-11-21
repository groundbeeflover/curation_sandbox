# SPDX-License-Identifier: Apache-2.0
# doc and world are exported: tools and commands take a doc::Document and transactions, and the
# camera controllers make world::Cameras.
cs_add_module(NAME editor DEPS core doc world geom light material assets engine EXPORTS doc world)
