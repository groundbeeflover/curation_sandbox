# SPDX-License-Identifier: Apache-2.0
#
# Checks of the golden-image runner, `cs-render golden` (M1-18), on copies of a golden test made in
# the build tree, so that nothing under tests/ is ever written:
#
#   CHECK=brighter  Acceptance criterion 3. The committed reference is a half-float EXR of the
#                   test's size and at most 60 KB. With every fixture type's flux_lm raised by 10%,
#                   the test fails: exit 1, the word "differs", the six failure images in --out,
#                   no warning (so the copied scene loaded whole and the failure is the brightness),
#                   and the test folder exactly as it was.
#   CHECK=update    Acceptance criterion 4, on a cheap copy (32 x 18, 4 samples per pixel):
#                   1. with no reference the test skips (exit 77) and writes nothing;
#                   2. --update writes a half-float reference, and nothing in the out folder;
#                   3. the next run (on 2 threads) passes, "matches", and writes nothing;
#                   4. seed 2 fails, "differs", writes the six images and leaves the reference;
#                   5. seed 1 again passes and removes the six images, and only them;
#                   6. --update with seed 2 rewrites the reference, after which seed 2 passes.
#   CHECK=errors    The exit codes for mistakes: 2 for a mistake on the command line, 1 for a
#                   test.json that is not valid or a scene that is missing; never 0 or 77, and
#                   nothing written.
#
# cmake -D CS_RENDER=<cs-render> -D GOLDEN_DIR=<absolute path of tests/golden/skeleton-path>
#       -D WORK_DIR=<scratch folder in the build tree, deleted and made again>
#       -D CHECK=brighter|update|errors -P runner_checks.cmake
#
# Only CMake's own commands, so that it runs the same on Linux and Windows.

cmake_minimum_required(VERSION 3.25)

# --- Arguments -------------------------------------------------------------------------------------

foreach(var CS_RENDER GOLDEN_DIR WORK_DIR CHECK)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "runner_checks.cmake: -D ${var}=... is required")
    endif()
endforeach()

if(NOT CHECK MATCHES "^(brighter|update|errors)$")
    message(FATAL_ERROR "runner_checks.cmake: CHECK is brighter, update or errors, not '${CHECK}'")
endif()

function(fail text)
    message(FATAL_ERROR "FAIL (${CHECK}): ${text}")
endfunction()

# --- The golden test this works from ---------------------------------------------------------------

file(READ "${GOLDEN_DIR}/test.json" golden_json)
foreach(key scene width height)
    string(JSON golden_${key} ERROR_VARIABLE error GET "${golden_json}" ${key})
    if(error)
        fail("${GOLDEN_DIR}/test.json: no ${key}: ${error}")
    endif()
endforeach()
set(scene_source "${golden_scene}")
cmake_path(ABSOLUTE_PATH scene_source BASE_DIRECTORY "${GOLDEN_DIR}" NORMALIZE)
if(NOT EXISTS "${scene_source}/project.json")
    fail("the scene of ${GOLDEN_DIR}/test.json, ${scene_source}, holds no project.json")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(scene_dir "${WORK_DIR}/scene")
set(test_dir "${WORK_DIR}/test")
set(out_dir "${WORK_DIR}/out")

# The six images the runner writes when a comparison fails, and removes when one passes.
set(failure_images render.exr render.png reference.png difference.exr difference.png flip.png)

# --- Running the runner ----------------------------------------------------------------------------

# Runs `cs-render golden <ARGN>` and prints what it printed. Sets `exit_code` and `output` (stdout
# and stderr together, in lower case, with Windows line ends made \n) in the caller. A sanitizer
# report fails the check whatever the exit code: AddressSanitizer and UndefinedBehaviorSanitizer
# exit with 1, which a check expecting a failed comparison would otherwise take for one.
function(run_golden what)
    string(JOIN " " shown ${ARGN})
    message(STATUS "${what}: cs-render golden ${shown}")
    execute_process(COMMAND "${CS_RENDER}" golden ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    string(REPLACE "\r" "" out "${out}")
    string(REPLACE "\r" "" err "${err}")
    message("  exit code: ${result}\n  stdout:\n${out}\n  stderr:\n${err}")
    if("${out}${err}" MATCHES
            "AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|[0-9]: runtime error: ")
        fail("${what}: a sanitizer reported an error (above)")
    endif()
    string(TOLOWER "${out}${err}" lower)
    set(exit_code "${result}" PARENT_SCOPE)
    set(output "${lower}" PARENT_SCOPE)
endfunction()

function(expect_exit what expected)
    if(NOT "${exit_code}" STREQUAL "${expected}")
        fail("${what}: cs-render golden exited with '${exit_code}', expected ${expected}")
    endif()
endfunction()

function(expect_word what word)
    string(FIND "${output}" "${word}" at)
    if(at EQUAL -1)
        fail("${what}: the output does not say '${word}'")
    endif()
endfunction()

function(expect_no_word what word)
    string(FIND "${output}" "${word}" at)
    if(NOT at EQUAL -1)
        fail("${what}: the output says '${word}'")
    endif()
endfunction()

# A run that compared and passed: exit 0, "matches", and no warning loading the scene.
function(expect_pass what)
    expect_exit("${what}" 0)
    expect_word("${what}" "matches")
    expect_no_word("${what}" "differs")
    expect_no_word("${what}" "warning")
endfunction()

# A run that compared and failed: exit 1, "differs", and no warning loading the scene.
function(expect_differs what)
    expect_exit("${what}" 1)
    expect_word("${what}" "differs")
    expect_no_word("${what}" "matches")
    expect_no_word("${what}" "warning")
endfunction()

# --- Files -----------------------------------------------------------------------------------------

# Every file and folder under `dir`, sorted: "<name>/" for a folder, "<name>=<SHA-256>" for a file.
# Empty if `dir` does not exist.
function(snapshot dir out)
    set(entries "")
    if(IS_DIRECTORY "${dir}")
        file(GLOB_RECURSE paths LIST_DIRECTORIES true RELATIVE "${dir}" "${dir}/*")
        list(SORT paths)
        foreach(path IN LISTS paths)
            if(IS_DIRECTORY "${dir}/${path}")
                list(APPEND entries "${path}/")
            else()
                file(SHA256 "${dir}/${path}" hash)
                list(APPEND entries "${path}=${hash}")
            endif()
        endforeach()
    endif()
    set(${out} "${entries}" PARENT_SCOPE)
endfunction()

function(describe_snapshot entries out)
    if(entries STREQUAL "")
        set(${out} "    (nothing)" PARENT_SCOPE)
    else()
        list(JOIN entries "\n    " joined)
        set(${out} "    ${joined}" PARENT_SCOPE)
    endif()
endfunction()

# Fails unless `dir` holds exactly the files and folders of the snapshot `before`, byte for byte.
function(expect_unchanged what dir before)
    snapshot("${dir}" after)
    if(NOT "${after}" STREQUAL "${before}")
        describe_snapshot("${before}" b)
        describe_snapshot("${after}" a)
        fail("${what}: ${dir} changed.\n  before:\n${b}\n  after:\n${a}")
    endif()
endfunction()

# Fails if `dir` exists and holds any file.
function(expect_no_files what dir)
    if(IS_DIRECTORY "${dir}")
        file(GLOB_RECURSE files LIST_DIRECTORIES false "${dir}/*")
        if(files)
            list(JOIN files "\n    " joined)
            fail("${what}: wrote into ${dir}, which should hold no files:\n    ${joined}")
        endif()
    endif()
endfunction()

# Fails unless `dir` holds the six failure images, each a PNG or EXR file as its name says. The
# images are the render's size by construction, so their headers are not read further.
function(expect_failure_images what dir)
    foreach(name IN LISTS failure_images)
        set(path "${dir}/${name}")
        if(NOT EXISTS "${path}")
            fail("${what}: ${name} was not written to ${dir}")
        endif()
        file(READ "${path}" magic LIMIT 8 HEX)
        if(name MATCHES "\\.png$" AND NOT magic STREQUAL "89504e470d0a1a0a")
            fail("${what}: ${path} is not a PNG file: it starts with ${magic}")
        elseif(name MATCHES "\\.exr$" AND NOT magic MATCHES "^762f3101")
            fail("${what}: ${path} is not an EXR file: it starts with ${magic}")
        endif()
    endforeach()
endfunction()

# --- EXR headers -----------------------------------------------------------------------------------
#
# Just enough of an OpenEXR header (openexr.com, "The OpenEXR File Layout") to check a reference:
# after the magic number 76 2f 31 01 and a 4-byte version come attributes, each a NUL-terminated
# name and type, a 32-bit little-endian size and the value. The file is read as hex, two digits a
# byte.

# The 32-bit little-endian signed integer at hex digit `pos` of `hex`, into `out`.
function(hex_int32 hex pos out)
    string(SUBSTRING "${hex}" ${pos} 8 little_endian)
    string(REGEX REPLACE "(..)(..)(..)(..)" "\\4\\3\\2\\1" big_endian "${little_endian}")
    math(EXPR value "0x${big_endian}")
    if(value GREATER_EQUAL 2147483648)
        math(EXPR value "${value} - 4294967296")
    endif()
    set(${out} ${value} PARENT_SCOPE)
endfunction()

# The first hex digit of `hex` at which the bytes `needle` (hex) start, on a byte boundary, or -1.
function(find_bytes hex needle out)
    set(start 0)
    while(TRUE)
        string(SUBSTRING "${hex}" ${start} -1 rest)
        string(FIND "${rest}" "${needle}" found)
        if(found EQUAL -1)
            set(${out} -1 PARENT_SCOPE)
            return()
        endif()
        math(EXPR at "${start} + ${found}")
        math(EXPR odd "${at} % 2")
        if(NOT odd)
            set(${out} ${at} PARENT_SCOPE)
            return()
        endif()
        math(EXPR start "${at} + 1")
    endwhile()
endfunction()

# The value, as hex, of the attribute `name` of type `type` in the header `hex` of `path`.
function(exr_attribute hex path name type out)
    string(HEX "${name}" name_hex)
    string(HEX "${type}" type_hex)
    set(key "${name_hex}00${type_hex}00")
    find_bytes("${hex}" "${key}" at)
    if(at EQUAL -1)
        fail("${path} has no EXR attribute ${name} of type ${type}")
    endif()
    string(LENGTH "${key}" key_length)
    math(EXPR size_at "${at} + ${key_length}")
    hex_int32("${hex}" ${size_at} size)
    math(EXPR value_at "${size_at} + 8")
    math(EXPR value_length "2 * ${size}")
    string(SUBSTRING "${hex}" ${value_at} ${value_length} value)
    set(${out} "${value}" PARENT_SCOPE)
endfunction()

# Fails unless `path` is an EXR file of width x height whose R, G and B are half floats.
function(expect_half_exr what path width height)
    if(NOT EXISTS "${path}")
        fail("${what}: ${path} does not exist")
    endif()
    # The header of an RGB file is about 300 bytes.
    file(READ "${path}" hex LIMIT 4096 HEX)
    if(NOT hex MATCHES "^762f3101")
        fail("${what}: ${path} is not an EXR file")
    endif()
    # Each channel of the list: its name, a NUL, then its pixel type, 1 for half and 2 for float.
    exr_attribute("${hex}" "${path}" channels chlist channels)
    foreach(channel R G B)
        string(HEX "${channel}" channel_hex)
        find_bytes("${channels}" "${channel_hex}0001000000" at)
        if(at EQUAL -1)
            fail("${what}: ${path} has no half-float ${channel} channel")
        endif()
    endforeach()
    # The data window: x min, y min, x max, y max.
    exr_attribute("${hex}" "${path}" dataWindow box2i window)
    foreach(i 0 1 2 3)
        math(EXPR pos "8 * ${i}")
        hex_int32("${window}" ${pos} corner${i})
    endforeach()
    math(EXPR exr_width "${corner2} - ${corner0} + 1")
    math(EXPR exr_height "${corner3} - ${corner1} + 1")
    if(NOT exr_width EQUAL width OR NOT exr_height EQUAL height)
        fail("${what}: ${path} is ${exr_width} x ${exr_height}, not ${width} x ${height}")
    endif()
endfunction()

# --- Test folders ----------------------------------------------------------------------------------

# Copies the golden test's scene folder, whole, to `destination`.
function(copy_scene destination)
    file(MAKE_DIRECTORY "${destination}")
    file(COPY "${scene_source}/" DESTINATION "${destination}")
    if(NOT EXISTS "${destination}/project.json")
        fail("copying ${scene_source} to ${destination} left no project.json")
    endif()
endfunction()

# GOLDEN_DIR's test.json with the scene `../scene`, for a test folder next to WORK_DIR/scene.
string(JSON copied_json SET "${golden_json}" scene [["../scene"]])

# The same, cheap: 32 x 18 at 4 samples per pixel. The tolerances: a mean FLIP of 0.01 is far above
# what rendering the same image twice on one machine gives (the half-float reference rounds by at
# most 2^-11 relative) and far below what another seed gives at 4 samples per pixel.
set(small_json "${copied_json}")
string(JSON small_json SET "${small_json}" width 32)
string(JSON small_json SET "${small_json}" height 18)
string(JSON small_json SET "${small_json}" samplesPerPixel 4)
string(JSON small_json SET "${small_json}" tolerances meanFlip 0.01)
string(JSON small_json SET "${small_json}" tolerances meanAbsoluteError_nits 0.5)
set(small_width 32)
set(small_height 18)

function(write_test_json dir json)
    file(MAKE_DIRECTORY "${dir}")
    file(WRITE "${dir}/test.json" "${json}\n")
endfunction()

# Multiplies every library.fixtureTypes[i].flux_lm of the project in `dir` by 1.1. CMake's math()
# has only integers, so each flux must be a whole number of lumens: 800.0 becomes 880.0 exactly.
function(raise_lumens dir)
    file(READ "${dir}/project.json" project)
    string(JSON count ERROR_VARIABLE error LENGTH "${project}" library fixtureTypes)
    if(error OR count EQUAL 0)
        fail("${dir}/project.json has no library.fixtureTypes, so no lumens to raise ${error}")
    endif()
    math(EXPR last "${count} - 1")
    foreach(i RANGE ${last})
        string(JSON flux ERROR_VARIABLE error GET "${project}" library fixtureTypes ${i} flux_lm)
        if(error)
            fail("${dir}/project.json: fixture type ${i} has no flux_lm to raise: ${error}")
        endif()
        if(NOT flux MATCHES "^([1-9][0-9]*)(\\.0*)?$")
            fail("${dir}/project.json: fixture type ${i} has flux_lm ${flux}, not a positive "
                "whole number of lumens this can multiply by 1.1")
        endif()
        # Times 11, then the decimal point before the last digit.
        math(EXPR tenths "${CMAKE_MATCH_1} * 11")
        string(REGEX REPLACE "(.)$" ".\\1" brighter "${tenths}")
        string(JSON project SET "${project}" library fixtureTypes ${i} flux_lm "${brighter}")
        message(STATUS "fixture type ${i}: flux_lm ${flux} -> ${brighter}")
    endforeach()
    file(WRITE "${dir}/project.json" "${project}\n")
endfunction()

# --- CHECK=brighter --------------------------------------------------------------------------------

if(CHECK STREQUAL "brighter")
    set(reference "${GOLDEN_DIR}/ref.cpu.exr")
    if(NOT EXISTS "${reference}")
        fail("${GOLDEN_DIR} has no reference, ref.cpu.exr, to compare the brighter render with")
    endif()
    # Acceptance criterion 3: a 16-bit half EXR of the test's size, of at most 60 KB, read as
    # 60,000 bytes, the stricter of 60 kB and 60 KiB.
    file(SIZE "${reference}" reference_bytes)
    if(reference_bytes GREATER 60000)
        fail("${reference} is ${reference_bytes} bytes, more than 60 KB")
    endif()
    expect_half_exr("the committed reference" "${reference}" ${golden_width} ${golden_height})
    message(STATUS "ok: ${reference} is a half-float EXR, ${golden_width} x ${golden_height}, "
        "${reference_bytes} bytes")

    copy_scene("${scene_dir}")
    raise_lumens("${scene_dir}")
    write_test_json("${test_dir}" "${copied_json}")
    file(COPY_FILE "${reference}" "${test_dir}/ref.cpu.exr")
    snapshot("${test_dir}" test_before)
    snapshot("${scene_dir}" scene_before)

    set(what "the spot 10% brighter")
    run_golden("${what}" "${test_dir}" --out "${out_dir}")
    expect_differs("${what}")
    expect_failure_images("${what}" "${out_dir}")
    expect_unchanged("${what}" "${test_dir}" "${test_before}")
    expect_unchanged("${what}" "${scene_dir}" "${scene_before}")
    message(STATUS "ok: ${what} fails, writes its six images and leaves its test folder as it was")
endif()

# --- CHECK=update ----------------------------------------------------------------------------------

if(CHECK STREQUAL "update")
    copy_scene("${scene_dir}")
    snapshot("${scene_dir}" scene_before)
    write_test_json("${test_dir}" "${small_json}")
    snapshot("${test_dir}" only_test_json)

    # 1. No reference: skipped, with nothing rendered or written.
    set(what "1. no reference")
    run_golden("${what}" "${test_dir}" --out "${out_dir}")
    expect_exit("${what}" 77)
    expect_unchanged("${what}" "${test_dir}" "${only_test_json}")
    expect_no_files("${what}" "${out_dir}")
    message(STATUS "ok: ${what}: skipped, nothing written")

    # 2. --update writes the reference, a half-float EXR of the test's size, and nothing else.
    set(what "2. --update with no reference")
    run_golden("${what}" "${test_dir}" --update --out "${out_dir}")
    expect_exit("${what}" 0)
    expect_no_word("${what}" "warning")
    expect_half_exr("${what}" "${test_dir}/ref.cpu.exr" ${small_width} ${small_height})
    file(SHA256 "${test_dir}/ref.cpu.exr" first_reference)
    # A snapshot is sorted by name: ref.cpu.exr comes before test.json.
    set(with_reference "ref.cpu.exr=${first_reference};${only_test_json}")
    expect_unchanged("${what}" "${test_dir}" "${with_reference}")
    expect_no_files("${what}" "${out_dir}")
    message(STATUS "ok: ${what}: wrote ref.cpu.exr and nothing else")

    # 3. The same render passes, on another number of threads, and writes nothing.
    set(what "3. the reference just written, on 2 threads")
    run_golden("${what}" "${test_dir}" --out "${out_dir}" --threads 2)
    expect_pass("${what}")
    expect_unchanged("${what}" "${test_dir}" "${with_reference}")
    expect_no_files("${what}" "${out_dir}")
    message(STATUS "ok: ${what}: passes, nothing written")

    # 4. Another seed: every path differs, so the comparison fails. The six images are written,
    #    and the test folder, the reference included, is left alone.
    string(JSON seed2_json SET "${small_json}" seed 2)
    write_test_json("${test_dir}" "${seed2_json}")
    snapshot("${test_dir}" seed2_before)
    set(what "4. seed 2")
    run_golden("${what}" "${test_dir}" --out "${out_dir}")
    expect_differs("${what}")
    expect_failure_images("${what}" "${out_dir}")
    expect_unchanged("${what}" "${test_dir}" "${seed2_before}")
    message(STATUS "ok: ${what}: fails, writes the six images, leaves the reference")

    # 5. Seed 1 again passes, and removes the six images, but nothing else in the out folder.
    write_test_json("${test_dir}" "${small_json}")
    file(WRITE "${out_dir}/keep.txt" "Not the runner's: a passing run removes only its six images.\n")
    file(SHA256 "${out_dir}/keep.txt" keep_hash)
    snapshot("${test_dir}" seed1_before)
    set(what "5. seed 1 again, on 1 thread")
    run_golden("${what}" "${test_dir}" --out "${out_dir}" --threads 1)
    expect_pass("${what}")
    expect_unchanged("${what}" "${test_dir}" "${seed1_before}")
    expect_unchanged("${what}" "${out_dir}" "keep.txt=${keep_hash}")
    message(STATUS "ok: ${what}: passes, removes the six images and only them")

    # 6. --update rewrites an existing reference; with it, seed 2 passes.
    write_test_json("${test_dir}" "${seed2_json}")
    file(SHA256 "${test_dir}/test.json" seed2_test_json)
    set(what "6. --update with seed 2")
    run_golden("${what}" "${test_dir}" --update --out "${out_dir}")
    expect_exit("${what}" 0)
    expect_no_word("${what}" "warning")
    expect_half_exr("${what}" "${test_dir}/ref.cpu.exr" ${small_width} ${small_height})
    file(SHA256 "${test_dir}/ref.cpu.exr" second_reference)
    if(second_reference STREQUAL first_reference)
        fail("${what}: ref.cpu.exr is the seed 1 reference still")
    endif()
    set(rewritten "ref.cpu.exr=${second_reference};test.json=${seed2_test_json}")
    expect_unchanged("${what}" "${test_dir}" "${rewritten}")
    expect_unchanged("${what}" "${out_dir}" "keep.txt=${keep_hash}")

    set(what "7. seed 2 against its new reference")
    run_golden("${what}" "${test_dir}" --out "${out_dir}")
    expect_pass("${what}")
    expect_unchanged("${what}" "${test_dir}" "${rewritten}")
    expect_unchanged("${what}" "${out_dir}" "keep.txt=${keep_hash}")
    message(STATUS "ok: --update rewrites a reference, and the next run passes against it")

    expect_unchanged("the runs above" "${scene_dir}" "${scene_before}")
endif()

# --- CHECK=errors ----------------------------------------------------------------------------------

if(CHECK STREQUAL "errors")
    copy_scene("${scene_dir}")
    snapshot("${scene_dir}" scene_before)

    # A valid test folder, small, with its reference: a run of it passes. Each case below starts
    # from it and changes one thing, so that a runner that missed the mistake would render, pass
    # and exit 0, and none can exit 77 for want of a reference.
    set(valid_dir "${WORK_DIR}/valid")
    write_test_json("${valid_dir}" "${small_json}")
    run_golden("the valid folder, --update" "${valid_dir}" --update --out "${out_dir}")
    expect_exit("the valid folder, --update" 0)
    run_golden("the valid folder" "${valid_dir}" --out "${out_dir}")
    expect_pass("the valid folder")
    expect_no_files("the valid folder" "${out_dir}")
    snapshot("${valid_dir}" valid_before)

    # Runs a case and checks it exits with `expected`, says something, and writes nothing.
    function(expect_refused what expected dir before)
        run_golden("${what}" ${ARGN})
        expect_exit("${what}" ${expected})
        if(output STREQUAL "")
            fail("${what}: exited with ${expected} without saying why")
        endif()
        expect_unchanged("${what}" "${dir}" "${before}")
        expect_no_files("${what}" "${out_dir}")
        message(STATUS "ok: ${what}: exit ${expected}, nothing written")
    endfunction()

    # Mistakes on the command line: exit 2.
    expect_refused("no test folder" 2 "${valid_dir}" "${valid_before}")
    expect_refused("an unknown option" 2 "${valid_dir}" "${valid_before}"
        "${valid_dir}" --no-such-option --out "${out_dir}")
    expect_refused("--out without a folder" 2 "${valid_dir}" "${valid_before}"
        "${valid_dir}" --out)
    expect_refused("--threads without a number" 2 "${valid_dir}" "${valid_before}"
        "${valid_dir}" --out "${out_dir}" --threads)
    expect_refused("--threads with a word" 2 "${valid_dir}" "${valid_before}"
        "${valid_dir}" --out "${out_dir}" --threads many)
    expect_refused("two test folders" 2 "${valid_dir}" "${valid_before}"
        "${valid_dir}" "${valid_dir}" --out "${out_dir}")

    # A test.json that is not valid, or a scene that is missing: exit 1, with --update or without.
    # Each case is a copy of the valid folder, reference included, with test.json replaced.
    function(refuse_test_json name json)
        string(MAKE_C_IDENTIFIER "${name}" slug)
        set(case_dir "${WORK_DIR}/case-${slug}")
        file(MAKE_DIRECTORY "${case_dir}")
        file(COPY_FILE "${valid_dir}/ref.cpu.exr" "${case_dir}/ref.cpu.exr")
        file(WRITE "${case_dir}/test.json" "${json}")
        snapshot("${case_dir}" before)
        expect_refused("test.json with ${name}" 1 "${case_dir}" "${before}"
            "${case_dir}" --out "${out_dir}")
        expect_refused("test.json with ${name}, and --update" 1 "${case_dir}" "${before}"
            "${case_dir}" --update --out "${out_dir}")
    endfunction()

    string(JSON json SET "${small_json}" colour [["blue"]])
    refuse_test_json("an unknown key" "${json}")
    string(JSON json SET "${small_json}" tolerances maxError 1.0)
    refuse_test_json("an unknown tolerance" "${json}")
    string(JSON json REMOVE "${small_json}" scene)
    refuse_test_json("no scene" "${json}")
    string(JSON json SET "${small_json}" scene [["../no-such-scene"]])
    refuse_test_json("a scene folder that does not exist" "${json}")
    string(JSON json SET "${small_json}" tier [["no-such-tier"]])
    refuse_test_json("an unknown tier" "${json}")
    string(JSON json SET "${small_json}" width 0)
    refuse_test_json("a width of 0" "${json}")
    string(JSON json SET "${small_json}" height -18)
    refuse_test_json("a negative height" "${json}")
    string(JSON json SET "${small_json}" samplesPerPixel 0)
    refuse_test_json("0 samples per pixel" "${json}")
    string(JSON json SET "${small_json}" width [["32"]])
    refuse_test_json("a width that is a string" "${json}")
    refuse_test_json("text that is not JSON" [[{ "scene": "../scene", ]])
    refuse_test_json("nothing in it" "")
    refuse_test_json("an array, not an object" "[]")

    # A folder with no test.json, and one that does not exist.
    set(empty_dir "${WORK_DIR}/no-test-json")
    file(MAKE_DIRECTORY "${empty_dir}")
    file(COPY_FILE "${valid_dir}/ref.cpu.exr" "${empty_dir}/ref.cpu.exr")
    snapshot("${empty_dir}" empty_before)
    expect_refused("a folder with no test.json" 1 "${empty_dir}" "${empty_before}"
        "${empty_dir}" --out "${out_dir}")
    set(missing_dir "${WORK_DIR}/no-such-folder")
    expect_refused("a test folder that does not exist" 1 "${missing_dir}" ""
        "${missing_dir}" --out "${out_dir}")
    if(EXISTS "${missing_dir}")
        fail("a test folder that does not exist: the runner made ${missing_dir}")
    endif()

    expect_unchanged("the runs above" "${valid_dir}" "${valid_before}")
    expect_unchanged("the runs above" "${scene_dir}" "${scene_before}")
endif()

message(STATUS "ok: every ${CHECK} check passed")
