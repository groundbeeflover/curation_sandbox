# SPDX-License-Identifier: Apache-2.0
# Re-exports beta, whose types its public header uses.
cs_add_module(NAME delta DEPS beta EXPORTS beta)
