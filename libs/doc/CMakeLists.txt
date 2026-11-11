# SPDX-License-Identifier: Apache-2.0
find_package(nlohmann_json CONFIG REQUIRED)

cs_add_module(NAME doc DEPS core EXTERNAL nlohmann_json::nlohmann_json)
