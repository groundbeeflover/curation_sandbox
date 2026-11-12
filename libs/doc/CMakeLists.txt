# SPDX-License-Identifier: Apache-2.0
find_package(nlohmann_json CONFIG REQUIRED)
find_package(miniz CONFIG REQUIRED)

# JSON for project.json and manifest.json, miniz for the .curation zip container.
cs_add_module(NAME doc DEPS core EXTERNAL nlohmann_json::nlohmann_json miniz::miniz)
