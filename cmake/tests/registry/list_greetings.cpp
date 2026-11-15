// SPDX-License-Identifier: Apache-2.0

#include <cs/greet/greet.hpp>

#include <iostream>
#include <string>
#include <vector>

int main() {
    std::vector<std::string> greetings;
    cs::greet::register_greetings(greetings, cs::greet::default_prefix());
    for (const std::string& greeting : greetings) {
        std::cout << greeting << '\n';
    }
    std::cout << greetings.size() << " greetings\n";
}
