# SPDX-License-Identifier: Apache-2.0
find_package(glm CONFIG REQUIRED)

cs_add_module(NAME core EXTERNAL glm::glm)
