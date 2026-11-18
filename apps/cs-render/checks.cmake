# SPDX-License-Identifier: Apache-2.0
#
# Checks of cs-render's GPU commands (M1-29, issue #90), registered in CMakeLists.txt:
#
#   CHECK=device-report  `cs-render --device-report` exits with 0 and prints the device report
#                        (cs::gpu::DeviceReport::to_string): device, driver, Vulkan 1.3 or later,
#                        ray query and validation, then the name of the device's golden
#                        references. In a debug build under CS_REQUIRE_GPU=1, validation is on
#                        (ADR 0011). With anything after it, it is a usage error: exit 2.
#   CHECK=standard       `cs-render <skeleton> --tier standard` exits with 0 and writes
#                        <prefix>.exr (a 32-bit float EXR of the size asked for), <prefix>.png and
#                        <prefix>.lux.json, whose tier is "standard", with one frame, the device it
#                        rendered on, and the chart's lux from the engine.
#
# Without a Vulkan device each prints "cs-render check skipped: no Vulkan device", which CTest
# shows as skipped (SKIP_REGULAR_EXPRESSION), unless CS_REQUIRE_GPU=1 is set: then it fails.
#
# cmake -D CS_RENDER=<cs-render> -D SCENE=<tests/scenes/skeleton> -D WORK_DIR=<scratch folder>
#       -D CONFIG=<Debug|Release|...> -D CHECK=device-report|standard -P checks.cmake

cmake_minimum_required(VERSION 3.25)

foreach(var CS_RENDER SCENE WORK_DIR CONFIG CHECK)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "checks.cmake: -D ${var}=... is required")
    endif()
endforeach()
if(NOT CHECK MATCHES "^(device-report|standard)$")
    message(FATAL_ERROR "checks.cmake: CHECK is device-report or standard, not '${CHECK}'")
endif()

function(fail text)
    message(FATAL_ERROR "FAIL (${CHECK}): ${text}")
endfunction()

# CS_REQUIRE_GPU=1, as gpu_test.hpp reads it: set, and neither empty nor "0".
set(gpu_required FALSE)
if(DEFINED ENV{CS_REQUIRE_GPU} AND NOT "$ENV{CS_REQUIRE_GPU}" STREQUAL ""
        AND NOT "$ENV{CS_REQUIRE_GPU}" STREQUAL "0")
    set(gpu_required TRUE)
endif()

# Runs cs-render with ARGN and prints what it printed. Sets `exit_code` and `output` (stdout and
# stderr together, Windows line ends made \n) in the caller. A sanitizer report fails the check
# whatever the exit code. With no Vulkan device the check stops here: skipped, or failed under
# CS_REQUIRE_GPU=1.
function(run_cs_render what)
    string(JOIN " " shown ${ARGN})
    message(STATUS "${what}: cs-render ${shown}")
    execute_process(COMMAND "${CS_RENDER}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    string(REPLACE "\r" "" out "${out}")
    string(REPLACE "\r" "" err "${err}")
    message("  exit code: ${result}\n  stdout:\n${out}\n  stderr:\n${err}")
    if("${out}${err}" MATCHES
            "AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|[0-9]: runtime error: ")
        fail("${what}: a sanitizer reported an error (above)")
    endif()
    if(NOT result EQUAL 0 AND "${err}" MATCHES "cs-render: no Vulkan device")
        if(gpu_required)
            fail("${what}: no Vulkan device, and CS_REQUIRE_GPU=1 requires one")
        endif()
        message("cs-render check skipped: no Vulkan device")
        # Stops the script without an error: CTest reads the line above as a skip.
        set(skipped TRUE PARENT_SCOPE)
        return()
    endif()
    set(skipped FALSE PARENT_SCOPE)
    set(exit_code "${result}" PARENT_SCOPE)
    set(output "${out}${err}" PARENT_SCOPE)
endfunction()

function(expect_exit what expected)
    if(NOT "${exit_code}" STREQUAL "${expected}")
        fail("${what}: cs-render exited with '${exit_code}', expected ${expected}")
    endif()
endfunction()

function(expect_match what regex)
    if(NOT output MATCHES "${regex}")
        fail("${what}: the output does not match '${regex}'")
    endif()
endfunction()

# --- CHECK=device-report ---------------------------------------------------------------------------

if(CHECK STREQUAL "device-report")
    set(what "--device-report")
    run_cs_render("${what}" --device-report)
    if(skipped)
        return()
    endif()
    expect_exit("${what}" 0)
    # Each line of DeviceReport::to_string, in its order, and the references' name.
    expect_match("${what}" "device: [^\n]+\ndriver: [^\n]+\nVulkan: 1\\.([3-9]|[1-9][0-9])\\.[0-9]+\n")
    expect_match("${what}" "\nray query: (yes|no)\nvalidation: (on|off)\n")
    expect_match("${what}" "\ngolden references: ref\\.[a-z0-9]+(-[a-z0-9]+)*\\.exr\n")
    if(gpu_required AND CONFIG STREQUAL "Debug")
        expect_match("${what}" "\nvalidation: on\n")
        message(STATUS "ok: ${what}: validation is on, as a debug build under CS_REQUIRE_GPU=1 needs")
    endif()
    message(STATUS "ok: ${what} prints the device report")

    set(what "--device-report with a project folder after it")
    run_cs_render("${what}" --device-report "${SCENE}")
    expect_exit("${what}" 2)
    message(STATUS "ok: ${what} is a usage error")
endif()

# --- CHECK=standard --------------------------------------------------------------------------------

if(CHECK STREQUAL "standard")
    file(REMOVE_RECURSE "${WORK_DIR}")
    file(MAKE_DIRECTORY "${WORK_DIR}")
    set(prefix "${WORK_DIR}/skeleton")
    set(what "--tier standard")
    # --spp is the path tier's; the Standard tier draws one frame whatever it says.
    run_cs_render("${what}" "${SCENE}" --tier standard --size 64x36 --spp 16 --lux-samples 64
        --out "${prefix}")
    if(skipped)
        return()
    endif()
    expect_exit("${what}" 0)
    expect_match("${what}" "Standard tier")

    foreach(suffix .exr .png .lux.json)
        if(NOT EXISTS "${prefix}${suffix}")
            fail("${what}: ${prefix}${suffix} was not written")
        endif()
    endforeach()
    file(READ "${prefix}.png" magic LIMIT 8 HEX)
    if(NOT magic STREQUAL "89504e470d0a1a0a")
        fail("${what}: ${prefix}.png is not a PNG file: it starts with ${magic}")
    endif()
    # The EXR: its magic number, and its data window (x min, y min, x max, y max as 32-bit
    # little-endian integers after the attribute's name, type and size) 64 x 36.
    file(READ "${prefix}.exr" hex LIMIT 4096 HEX)
    if(NOT hex MATCHES "^762f3101")
        fail("${what}: ${prefix}.exr is not an EXR file")
    endif()
    # "dataWindow", NUL, "box2i", NUL, the size 16, then 0, 0, 63 and 35.
    string(HEX "dataWindow" name_hex)
    string(HEX "box2i" type_hex)
    set(window "10000000" "00000000" "00000000" "3f000000" "23000000")
    string(JOIN "" window ${window})
    if(NOT hex MATCHES "${name_hex}00${type_hex}00${window}")
        fail("${what}: ${prefix}.exr is not 64 x 36")
    endif()

    file(READ "${prefix}.lux.json" report)
    # Sets `value` to the report's member at the path ARGN, or fails.
    function(report_value)
        string(JSON found ERROR_VARIABLE error GET "${report}" ${ARGN})
        if(error)
            fail("${what}: ${prefix}.lux.json: ${error}")
        endif()
        set(value "${found}" PARENT_SCOPE)
    endfunction()
    report_value(tier)
    if(NOT value STREQUAL "standard")
        fail("${what}: the lux report's tier is '${value}', not standard")
    endif()
    report_value(image width)
    set(width "${value}")
    report_value(image height)
    if(NOT "${width}x${value}" STREQUAL "64x36")
        fail("${what}: the lux report's image is ${width} x ${value}, not 64 x 36")
    endif()
    report_value(image samplesPerPixel)
    if(NOT value STREQUAL "1")
        fail("${what}: the Standard tier draws one frame, but the report says ${value}")
    endif()
    report_value(device name)
    if(value STREQUAL "")
        fail("${what}: the lux report names no device")
    endif()
    # The skeleton's chart, from the engine: about 1,009 lux at its centre (its README), and a
    # few hundred over its grid on average.
    report_value(exhibits 0 direct mean_lx)
    if(NOT value GREATER 100)
        fail("${what}: the chart's mean direct lux is ${value}")
    endif()
    message(STATUS "ok: ${what} writes the EXR, the PNG and the lux report")
endif()

message(STATUS "ok: every ${CHECK} check passed")
