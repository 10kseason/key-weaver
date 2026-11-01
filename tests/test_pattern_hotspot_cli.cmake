if(NOT DEFINED KEYWEAVER_EXE OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "KEYWEAVER_EXE and TEST_ROOT are required")
endif()
cmake_path(ABSOLUTE_PATH TEST_ROOT NORMALIZE OUTPUT_VARIABLE root)
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run_id)
set(work "${root}/pattern-hotspot-cli-${run_id}")
file(MAKE_DIRECTORY "${work}")

function(run_ok label)
    execute_process(COMMAND "${KEYWEAVER_EXE}" ${ARGN}
        WORKING_DIRECTORY "${work}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "${label}: ${result}\n${output}\n${error}")
    endif()
endfunction()

function(run_rejected label expected)
    execute_process(COMMAND "${KEYWEAVER_EXE}" ${ARGN}
        WORKING_DIRECTORY "${work}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
    if(NOT result MATCHES "^[0-9]+$" OR result STREQUAL "0" OR NOT error MATCHES "${expected}")
        message(FATAL_ERROR "${label} did not reject explicitly: ${result}\n${output}\n${error}")
    endif()
endfunction()

function(same_bytes label a b)
    file(SHA256 "${a}" a_sha)
    file(SHA256 "${b}" b_sha)
    if(NOT a_sha STREQUAL b_sha)
        message(FATAL_ERROR "${label}: bytes changed")
    endif()
endfunction()

set(header "osu file format v14\n[General]\nMode:3\n[Metadata]\nTitle:Hotspot fixture\nVersion:Native\n[Difficulty]\nCircleSize:7\n[TimingPoints]\n0,500,4,2,1,70,1,0\n[HitObjects]\n")
set(objects "")
foreach(i RANGE 0 63)
    math(EXPR time "1000 + ${i} * 500")
    math(EXPR lane "${i} % 7")
    math(EXPR x "(2 * ${lane} + 1) * 512 / 14")
    math(EXPR hold "${i} % 8")
    if(hold EQUAL 0)
        math(EXPR end "${time} + 1250")
        string(APPEND objects "${x},192,${time},128,0,${end}:0:0:0:0:\n")
    else()
        string(APPEND objects "${x},192,${time},1,0,0:0:0:0:\n")
    endif()
endforeach()
set(input "${work}/native.osu")
file(WRITE "${input}" "${header}${objects}")
file(SHA256 "${input}" input_before)
set(base "${input}" --engine nk2 --target 10)
set(explicit_e --nk2-mode faithful --nk2-search phrase --nk2-phrase-support
    --nk2-phrase-dense-ln --nk2-phrase-source-ln)

run_ok(off ${base} --out "${work}/off.osu" --report "${work}/off.json")
run_ok(on ${base} --out "${work}/on.osu" --report "${work}/on.json"
    --pattern-hotspot-report "${work}/audit.json")
same_bytes("converted chart" "${work}/off.osu" "${work}/on.osu")
same_bytes("existing E report" "${work}/off.json" "${work}/on.json")
run_ok(explicit_e ${base} ${explicit_e} --dry-run --out "${work}/dry.osu"
    --pattern-hotspot-report "${work}/repeat.json")
same_bytes("automatic/explicit E and repeat diagnostic" "${work}/audit.json" "${work}/repeat.json")
if(EXISTS "${work}/dry.osu")
    message(FATAL_ERROR "dry-run wrote a chart")
endif()
file(READ "${work}/audit.json" diagnostic)
file(READ "${work}/on.json" report)
string(JSON preset GET "${diagnostic}" preset)
string(JSON stage GET "${diagnostic}" analysisStage)
string(JSON count GET "${diagnostic}" patternHotspots totalNotes)
string(JSON input_count GET "${diagnostic}" patternHotspots inputNotes)
string(JSON key_count GET "${diagnostic}" patternHotspots laneCount)
string(JSON expected_count GET "${report}" placement outputNotes)
string(JSON passed GET "${report}" independentSafety passed)
string(JSON coverage GET "${report}" independentSafety sourceLnCoverageRequired)
string(JSON violations GET "${report}" independentSafety generatedHoldsOutsideSourceLn)
string(JSON generated_holds GET "${report}" placement addedHoldNotes)
if(NOT preset STREQUAL "E" OR NOT stage STREQUAL "final_chart_before_export" OR
   NOT count EQUAL expected_count OR NOT input_count EQUAL count OR NOT key_count EQUAL 10 OR
   NOT passed OR NOT coverage OR NOT violations EQUAL 0 OR NOT generated_holds GREATER 0)
    message(FATAL_ERROR "Diagnostic must count the safe E result, including generated holds")
endif()
set(total 0)
foreach(lane RANGE 0 9)
    string(JSON observed GET "${diagnostic}" patternHotspots globalLaneCounts ${lane})
    string(JSON expected GET "${report}" placement laneDistribution ${lane})
    if(NOT observed EQUAL expected)
        message(FATAL_ERROR "Global lane count differs from existing E report")
    endif()
    math(EXPR total "${total} + ${observed}")
endforeach()
string(JSON windows LENGTH "${diagnostic}" patternHotspots windows)
math(EXPR last_window "${windows} - 1")
set(window_total 0)
foreach(index RANGE 0 ${last_window})
    string(JSON n GET "${diagnostic}" patternHotspots windows ${index} noteCount)
    math(EXPR window_total "${window_total} + ${n}")
endforeach()
if(NOT total EQUAL count OR NOT window_total EQUAL count)
    message(FATAL_ERROR "Window/global totals must preserve all final note heads")
endif()

# A pre-change executable can be supplied by a local verification run.
if(DEFINED BASELINE_EXE)
    execute_process(COMMAND "${BASELINE_EXE}" ${base}
        --out "${work}/baseline.osu" --report "${work}/baseline.json"
        RESULT_VARIABLE baseline_result OUTPUT_VARIABLE baseline_out ERROR_VARIABLE baseline_error TIMEOUT 10)
    if(NOT baseline_result STREQUAL "0")
        message(FATAL_ERROR "Baseline failed: ${baseline_out}\n${baseline_error}")
    endif()
    same_bytes("pre-change E chart" "${work}/baseline.osu" "${work}/on.osu")
    same_bytes("pre-change E report" "${work}/baseline.json" "${work}/on.json")
endif()

run_rejected(existing "existing files are never overwritten" ${base} --dry-run
    --pattern-hotspot-report "${work}/audit.json")
same_bytes("existing sidecar preserved" "${work}/audit.json" "${work}/repeat.json")
run_rejected(alias_report "must differ" ${base} --dry-run
    --pattern-hotspot-report "${work}/alias.json" --report "${work}/./alias.json")
run_rejected(alias_chart "must differ" ${base}
    --pattern-hotspot-report "${work}/alias.json" --out "${work}/./alias.json")
run_rejected(relative_report "must differ" ${base} --dry-run
    --pattern-hotspot-report relative.json --report ./relative.json)
run_rejected(relative_chart "must differ" ${base}
    --pattern-hotspot-report relative.json --out "${work}/relative.json")
if(WIN32)
    run_rejected(case_alias "must differ" ${base} --dry-run
        --pattern-hotspot-report "${work}/case.json" --report "${work}/CASE.JSON")
    run_rejected(win32_alias "aliases another output|must differ" ${base}
        --pattern-hotspot-report "${work}/trailing.json" --report "${work}/trailing.json.")
    if(EXISTS "${work}/trailing.json")
        file(READ "${work}/trailing.json" trailing_json)
        string(JSON preserved_preset GET "${trailing_json}" preset)
        if(NOT preserved_preset STREQUAL "E")
            message(FATAL_ERROR "A Win32 filename alias overwrote the diagnostic")
        endif()
    endif()
    run_rejected(alternate_stream "alternate data stream" ${base} --dry-run
        --pattern-hotspot-report "${work}/native.osu:diagnostic.json")
endif()
run_rejected(source_path "new .json" ${base} --dry-run --pattern-hotspot-report "${input}")
run_rejected(other_preset "effective NK2 preset E" ${base} --nk2-mode faithful --nk2-search off
    --dry-run --pattern-hotspot-report "${work}/bad.json")
run_rejected(f_boost "effective NK2 preset E" ${base} ${explicit_e} --nk2-phrase-source-ln-boost
    --dry-run --pattern-hotspot-report "${work}/bad.json")
run_rejected(classic "one NK2 preset E" "${input}" --target 10 --dry-run
    --pattern-hotspot-report "${work}/bad.json")
run_rejected(other_target "one NK2 preset E" "${input}" --engine nk2 --target 7 --dry-run
    --pattern-hotspot-report "${work}/bad.json")
run_rejected(batch "one NK2 preset E" ${base} --batch --dry-run
    --pattern-hotspot-report "${work}/bad.json")
if(EXISTS "${work}/bad.json" OR EXISTS "${work}/alias.json")
    message(FATAL_ERROR "Rejected requests created output")
endif()

# Invalid partial populations are reported separately without changing E output.
file(WRITE "${work}/warning.osu" "${header}${objects}bad-hit-object\n")
run_ok(parser_warning "${work}/warning.osu" --engine nk2 --target 10 --dry-run
    --pattern-hotspot-report "${work}/warning.json")
file(READ "${work}/warning.json" warning_json)
string(JSON warning_status GET "${warning_json}" patternHotspots status)
string(JSON warning_reason GET "${warning_json}" patternHotspots reasonCode)
if(NOT warning_status STREQUAL "invalid_input" OR NOT warning_reason STREQUAL "parser_warnings")
    message(FATAL_ERROR "Parser omissions must not masquerade as a clean audit")
endif()
file(WRITE "${work}/empty.osu" "${header}")
run_ok(empty "${work}/empty.osu" --engine nk2 --target 10 --dry-run
    --pattern-hotspot-report "${work}/empty.json")
file(READ "${work}/empty.json" empty_json)
string(JSON empty_status GET "${empty_json}" patternHotspots status)
if(NOT empty_status STREQUAL "no_data")
    message(FATAL_ERROR "Empty final E charts must be explicit no_data")
endif()

# BMS is observed in memory before export; no sparse-output reparse is used.
file(WRITE "${work}/native.bms"
    "#PLAYER 1\n#TITLE Hotspot BMS\n#BPM 120\n#WAV01 sample.wav\n#00111:0100\n#00112:0001\n#00213:0100\n#00214:0001\n#00315:0100\n#00318:0001\n#00419:0100\n#00551:01000001\n")
run_ok(bms_off "${work}/native.bms" --source 7 --engine nk2 --target 10
    --out "${work}/off.bms" --report "${work}/off-bms.json")
run_ok(bms_on "${work}/native.bms" --source 7 --engine nk2 --target 10
    --out "${work}/on.bms" --report "${work}/on-bms.json"
    --pattern-hotspot-report "${work}/bms-audit.json")
same_bytes("BMS output unchanged" "${work}/off.bms" "${work}/on.bms")
same_bytes("BMS report unchanged" "${work}/off-bms.json" "${work}/on-bms.json")
file(READ "${work}/bms-audit.json" bms_json)
file(READ "${work}/on-bms.json" bms_report)
string(JSON bms_status GET "${bms_json}" patternHotspots status)
string(JSON bms_count GET "${bms_json}" patternHotspots totalNotes)
string(JSON bms_expected GET "${bms_report}" placement outputNotes)
if(bms_status STREQUAL "invalid_input" OR NOT bms_count EQUAL bms_expected)
    message(FATAL_ERROR "BMS audit must describe the normalized final E Chart")
endif()
file(SHA256 "${input}" input_after)
if(NOT input_before STREQUAL input_after)
    message(FATAL_ERROR "Input chart was modified")
endif()

# Only this fresh random child belongs to the test; retain it on earlier failure.
cmake_path(IS_PREFIX root "${work}" NORMALIZE inside_root)
if(NOT inside_root OR work STREQUAL root)
    message(FATAL_ERROR "Refusing cleanup outside test root")
endif()
file(REMOVE_RECURSE "${work}")
message(STATUS "Pattern hotspot CLI integration passed")
