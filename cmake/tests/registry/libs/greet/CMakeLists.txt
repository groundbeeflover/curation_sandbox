# SPDX-License-Identifier: Apache-2.0
cs_add_module(NAME greet)

# Every src/greetings/<name>.cpp defines cs::greet::register_<name>(names, prefix).
cs_generate_registry(greet greetings
    PARAMETERS "std::vector<std::string>&" "const std::string&"
    INCLUDES "cs/greet/greet.hpp")
