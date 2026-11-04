# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Sedge, measured through the built plugin (dsp.test): a sine passes
# unchanged and reads as its note, octave, cents and frequency, in equal
# temperament and in Werckmeister III. (host-smoke checks that every sample
# passes exactly, as a transparent product.)
# Required: SANDBOX (steppe-sandbox), PLUGIN (Sedge.clap), WORK.
foreach(var SANDBOX PLUGIN WORK)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "response: -D${var}=... is required")
    endif()
endforeach()
# A decimal as an integer number of thousandths (CMake's math has no decimals).
function(milli value out)
    if(value MATCHES "e")
        set(${out} 0 PARENT_SCOPE)  # a tiny value in exponent form
        return()
    endif()
    if(NOT value MATCHES "^(-?)([0-9]+)(\\.([0-9]*))?$")
        message(FATAL_ERROR "not a number: ${value}")
    endif()
    set(sign "${CMAKE_MATCH_1}")
    set(whole "${CMAKE_MATCH_2}")
    string(SUBSTRING "${CMAKE_MATCH_4}000" 0 3 fraction)
    math(EXPR m "${whole} * 1000 + 1${fraction} - 1000")
    if(sign STREQUAL "-")
        math(EXPR m "-${m}")
    endif()
    set(${out} ${m} PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK}")
set(home "${WORK}/home")
file(MAKE_DIRECTORY "${home}")

# A second of a sine at `hz`, with the patch's parameters (a JSON object).
function(measure hz params out)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "HOME=${home}" "USERPROFILE=${home}" "APPDATA=${home}/config"
                "LOCALAPPDATA=${home}/local" "XDG_DATA_HOME=${home}/data" "${SANDBOX}" exec
                "{\"command\":\"dsp.test\",\"args\":{\"plugin\":\"${PLUGIN}\",\"seconds\":1,\"frequency_hz\":${hz},\"level_db\":-12,\"signals\":[\"sine\"],\"patch\":{\"params\":${params}}}}"
                --workdir "${WORK}/workdir"
        RESULT_VARIABLE result OUTPUT_VARIABLE json ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "dsp.test failed (${result}):\n${json}\n${err}")
    endif()
    set(${out} "${json}" PARENT_SCOPE)
endfunction()

# A value from the result, within `tolerance` thousandths of `want`.
function(expect what value want tolerance)
    milli("${value}" got)
    milli("${want}" wanted)
    math(EXPR off "${got} - ${wanted}")
    if(off GREATER ${tolerance} OR off LESS -${tolerance})
        message(FATAL_ERROR "${what} reads ${value}, want ${want}")
    endif()
    message(STATUS "${what}: ${value}")
endfunction()
function(expect_reading json note octave cents hz)
    string(JSON gain GET "${json}" signals 0 gain_db)
    expect(gain_db "${gain}" 0 0)
    string(JSON got GET "${json}" signals 0 meters note)
    if(NOT got STREQUAL "${note}")
        message(FATAL_ERROR "the note reads ${got}, want ${note}")
    endif()
    string(JSON got GET "${json}" signals 0 meters octave)
    expect(octave "${got}" ${octave} 0)
    string(JSON got GET "${json}" signals 0 meters cents)
    expect(cents "${got}" ${cents} 50)
    string(JSON got GET "${json}" signals 0 meters frequency)
    expect(frequency "${got}" ${hz} 20)
endfunction()

# A4 at 445 Hz is 19.56 cents sharp of 440.
measure(445 "{}" json)
expect_reading("${json}" a 4 19.56 445)
# Werckmeister III on C puts E 1.96 cents above equal, A kept at 440: an
# equal-tempered E4 reads that much flat.
measure(329.6276 "{\"temperament\":\"werckmeister3\",\"key\":\"c\"}" json)
expect_reading("${json}" e 4 -1.955 329.6276)
