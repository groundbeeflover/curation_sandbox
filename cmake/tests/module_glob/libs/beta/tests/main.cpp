// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_session.hpp>

bool g_custom_main_ran = false;

int main(int argc, char* argv[]) {
    g_custom_main_ran = true;
    return Catch::Session().run(argc, argv);
}
