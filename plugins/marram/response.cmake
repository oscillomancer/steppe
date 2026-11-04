# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Marram, measured through the built plugin (dsp.test's A4 note and silence):
# every pitched oscillator type, a unison stack and every oversampling play the
# note in tune (a sub an octave down), routes from a global source (a macro)
# and a voice's own (velocity) move it as far as their amounts say, and
# nothing is wrong with the sound or with the silence after it (no clipping,
# DC, sound that never ends).
# Required: SANDBOX (steppe-sandbox), PLUGIN (Marram.clap), WORK.
foreach(var SANDBOX PLUGIN WORK)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "response: -D${var}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
set(home "${WORK}/home")
file(MAKE_DIRECTORY "${home}")

# The note's pitch with these settings (and routes, when given), which must
# be within 5 cents of `hz` (CMake's math has no decimals: 0.3 % is 5 cents).
function(expect params hz)
    set(routes "")
    if(ARGC GREATER 2)
        set(routes ",\"routes\":${ARGV2}")
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "HOME=${home}" "USERPROFILE=${home}" "APPDATA=${home}/config"
                "LOCALAPPDATA=${home}/local" "XDG_DATA_HOME=${home}/data" "${SANDBOX}" exec
                "{\"command\":\"dsp.test\",\"args\":{\"plugin\":\"${PLUGIN}\",\"seconds\":2,\"signals\":[\"note\",\"silence\"],\"patch\":{\"params\":${params}${routes}}}}"
                --workdir "${WORK}/workdir"
        RESULT_VARIABLE result OUTPUT_VARIABLE json ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "dsp.test failed (${result}) for ${params}:\n${json}\n${err}")
    endif()
    foreach(i 0 1)
        string(JSON problems ERROR_VARIABLE none GET "${json}" signals ${i} problems)
        if(NOT none AND NOT problems STREQUAL "" AND NOT problems STREQUAL "[]")
            message(FATAL_ERROR "${params}: ${problems}")
        endif()
    endforeach()
    string(JSON pitch GET "${json}" signals 0 pitch_hz)
    string(REGEX REPLACE "\\..*" "" whole "${pitch}")
    math(EXPR low "${hz} * 997 / 1000")
    math(EXPR high "${hz} * 1003 / 1000")
    if(whole LESS low OR whole GREATER high)
        message(FATAL_ERROR "${params}: the A4 note sounds at ${pitch} Hz, want ${hz}")
    endif()
    message(STATUS "${params}: ${pitch} Hz")
endfunction()

expect("{}" 440)
expect("{\"osc1.wave\":\"sine\",\"filter1.type\":\"off\"}" 440)
expect("{\"osc1.type\":\"wavetable\",\"osc1.table\":\"pwm\",\"osc1.position\":40}" 440)
expect("{\"osc1.type\":\"fm\",\"osc1.fm\":1.5,\"osc2.type\":\"fm\",\"osc2.ratio\":2,\"osc2.level\":-60}" 440)
expect("{\"osc1.type\":\"sub\",\"osc1.wave\":\"sine\",\"filter1.type\":\"off\"}" 220)
expect("{\"voice.unison\":3,\"voice.unison_detune\":10}" 440)
expect("{\"voice.oversampling\":\"x1\"}" 440)
expect("{\"voice.oversampling\":\"x4\",\"filter1.drive\":24}" 440)
# A semitone up by Macro 1 at full; 80 cents up by dsp.test's velocity, 0.8.
expect("{\"macro1\":100}" 466 "{\"macro1>voice.tune\":{\"amount\":\"1 st\"}}")
expect("{}" 460 "{\"velocity>voice.tune\":{\"amount\":100}}")

# The tail it reports is its release, so a host that ends a render on it
# keeps the whole release: a 1 s release renders past its key's release
# (a half note, ending at 1 s) for 1 s and its 20 ms margin.
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "HOME=${home}" "USERPROFILE=${home}" "APPDATA=${home}/config"
            "LOCALAPPDATA=${home}/local" "XDG_DATA_HOME=${home}/data" "${SANDBOX}" exec
            "{\"command\":\"batch\",\"args\":{\"steps\":[{\"command\":\"plugin.load\",\"args\":{\"plugin\":\"${PLUGIN}\"}},{\"command\":\"patch.merge\",\"args\":{\"instance\":\"in_1\",\"patch\":{\"params\":{\"amp.release\":1000}}}},{\"command\":\"render\",\"args\":{\"notation\":\"1: A4:h r:h\",\"seconds\":1,\"tail_s\":5}}]}}"
            --workdir "${WORK}/workdir"
    RESULT_VARIABLE result OUTPUT_VARIABLE json ERROR_VARIABLE err)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the tail render failed (${result}):\n${json}\n${err}")
endif()
string(JSON tail GET "${json}" results 2 tail_s)
if(NOT tail STREQUAL "1.02")
    message(FATAL_ERROR "a 1 s release renders a ${tail} s tail, want 1.02")
endif()
message(STATUS "a 1 s release: a ${tail} s tail")
