# SPDX-License-Identifier: Apache-2.0
# Golden-image tests: every <name>/test.json here is one (cmake/CsGolden.cmake, README.md).
cs_add_golden_tests()

# Checks of the runner itself, on copies of skeleton-path in the build tree (runner_checks.cmake):
# with the spot 10% brighter the test fails and writes its images; a missing reference skips;
# --update writes one; nothing else writes into a test folder; and mistakes in test.json or on the
# command line are errors. They run with the goldens (`ctest -L golden`).
set(cs_golden_checks
    "brighter|skeleton-path fails with the spot 10 percent brighter"
    "update|a missing reference skips, --update writes one, nothing else writes the test folder"
    "errors|a test.json or command line with a mistake is an error, and writes nothing")
foreach(entry IN LISTS cs_golden_checks)
    string(REPLACE "|" ";" entry "${entry}")
    list(GET entry 0 check)
    list(GET entry 1 description)
    add_test(NAME "golden runner: ${description}"
        COMMAND ${CMAKE_COMMAND}
            -D "CS_RENDER=$<TARGET_FILE:cs-render>"
            -D "GOLDEN_DIR=${CMAKE_CURRENT_SOURCE_DIR}/skeleton-path"
            -D "WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/runner-${check}"
            -D "CHECK=${check}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/runner_checks.cmake")
    set_tests_properties("golden runner: ${description}" PROPERTIES LABELS golden TIMEOUT 300)
endforeach()
