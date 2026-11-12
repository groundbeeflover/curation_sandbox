# SPDX-License-Identifier: Apache-2.0
# Declares delta only, and sees beta because delta re-exports it.
cs_add_module(NAME zeta DEPS delta)
