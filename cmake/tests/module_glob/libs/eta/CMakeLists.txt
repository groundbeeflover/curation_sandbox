# SPDX-License-Identifier: Apache-2.0
# Declares epsilon only, and sees beta because the header-only epsilon re-exports it.
cs_add_module(NAME eta DEPS epsilon)
