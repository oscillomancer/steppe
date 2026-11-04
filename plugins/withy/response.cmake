# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Withy, measured through the built plugin (dsp.test), whose sine peaks at
# -12 dBFS on both channels. Dry (mix 0) it passes unchanged; output trims
# what leaves; one voice held still (vibrato at no depth) is the sine at its
# own level; low keep returns the lows whole, however the voices sway. The
# readouts agree with the depth, the shape and the rate as set.
# Required: SANDBOX (steppe-sandbox), PLUGIN (Withy.clap), WORK.
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

function(measure hz params out)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "HOME=${home}" "USERPROFILE=${home}" "APPDATA=${home}/config"
                "LOCALAPPDATA=${home}/local" "XDG_DATA_HOME=${home}/data" "${SANDBOX}" exec
                "{\"command\":\"dsp.test\",\"args\":{\"plugin\":\"${PLUGIN}\",\"seconds\":2,\"frequency_hz\":${hz},\"level_db\":-12,\"signals\":[\"sine\"],\"patch\":{\"params\":${params}}}}"
                --workdir "${WORK}/workdir"
        RESULT_VARIABLE result OUTPUT_VARIABLE json ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "dsp.test failed (${result}) for ${params}:\n${json}\n${err}")
    endif()
    set(${out} "${json}" PARENT_SCOPE)
endfunction()

# The sine's gain at hz with these settings, which must be within 0.01 dB of `want`.
function(expect hz params want)
    measure(${hz} "${params}" json)
    string(JSON gain GET "${json}" signals 0 gain_db)
    milli("${gain}" got)
    milli("${want}" wanted)
    math(EXPR off "${got} - ${wanted}")
    if(off GREATER 10 OR off LESS -10)
        message(FATAL_ERROR "${params}: a ${hz} Hz sine's gain is ${gain} dB, want ${want}")
    endif()
    message(STATUS "${params}: gain ${gain} dB at ${hz} Hz")
endfunction()

expect(1000 "{\"mix\":0}" 0)
expect(1000 "{\"mix\":0,\"output\":-6}" -6)
expect(1000 "{\"style\":\"vibrato\",\"depth\":0}" 0)
# A full vibrato on everything above 1 kHz, and a 60 Hz tone untouched.
expect(60 "{\"style\":\"vibrato\",\"depth\":100,\"rate\":6,\"low_keep\":1000}" 0)

# A readout, within `tolerance` thousandths of `want`.
function(expect_meter params key want tolerance)
    measure(1000 "${params}" json)
    string(JSON value GET "${json}" signals 0 meters ${key})
    milli("${value}" got)
    milli("${want}" wanted)
    math(EXPR off "${got} - ${wanted}")
    if(off GREATER ${tolerance} OR off LESS -${tolerance})
        message(FATAL_ERROR "${params}: ${key} reads ${value}, want ${want}")
    endif()
    message(STATUS "${params}: ${key} ${value}")
endfunction()

# The depth is the pitch each voice swings, so the sweep follows from it:
# 12 ct at 1 Hz is 2^(12/1200) - 1 = 0.6956% of a second over 2 pi for a
# sine, 1.107 ms; the rounded triangle's gentler slope (4.25 a cycle) makes
# it 1.636 ms.
expect_meter("{\"depth\":12,\"rate\":1,\"shape\":\"sine\"}" detune 12 5)
expect_meter("{\"depth\":12,\"rate\":1,\"shape\":\"sine\"}" sweep 1.107 2)
expect_meter("{\"depth\":12,\"rate\":1,\"shape\":\"triangle\"}" sweep 1.636 2)
# A quarter note at 120 BPM (the tempo when the host sends none).
expect_meter("{\"sync\":\"n4\"}" rate_now 2 1)
