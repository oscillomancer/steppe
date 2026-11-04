# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Pollia, measured through the built plugin (dsp.test), whose 1 kHz sine
# peaks at -12 dBFS on both channels. Dry (mix 0) it passes unchanged; two
# stages held at 1 kHz cancel it (a notch exactly at their corner), and with
# feedback leave what the loop's analytic response says; keep below at the
# notch passes half; the wet alone is an all-pass, swept or turning (the
# 90-degree pair keeps the level as the phase turns).
# Required: SANDBOX (steppe-sandbox), PLUGIN (Pollia.clap), WORK.
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

function(measure params out)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "HOME=${home}" "USERPROFILE=${home}" "APPDATA=${home}/config"
                "LOCALAPPDATA=${home}/local" "XDG_DATA_HOME=${home}/data" "${SANDBOX}" exec
                "{\"command\":\"dsp.test\",\"args\":{\"plugin\":\"${PLUGIN}\",\"seconds\":2,\"frequency_hz\":1000,\"level_db\":-12,\"signals\":[\"sine\"],\"patch\":{\"params\":${params}}}}"
                --workdir "${WORK}/workdir"
        RESULT_VARIABLE result OUTPUT_VARIABLE json ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "dsp.test failed (${result}) for ${params}:\n${json}\n${err}")
    endif()
    set(${out} "${json}" PARENT_SCOPE)
endfunction()

# The sine's gain with these settings, within `tolerance` thousandths of a dB of `want`.
function(expect params want tolerance)
    measure("${params}" json)
    string(JSON gain GET "${json}" signals 0 gain_db)
    milli("${gain}" got)
    milli("${want}" wanted)
    math(EXPR off "${got} - ${wanted}")
    if(off GREATER ${tolerance} OR off LESS -${tolerance})
        message(FATAL_ERROR "${params}: a 1 kHz sine's gain is ${gain} dB, want ${want}")
    endif()
    message(STATUS "${params}: gain ${gain} dB")
endfunction()

# The sine's gain, below `ceiling` dB.
function(expect_below params ceiling)
    measure("${params}" json)
    string(JSON gain GET "${json}" signals 0 gain_db)
    milli("${gain}" got)
    milli("${ceiling}" top)
    if(got GREATER top)
        message(FATAL_ERROR "${params}: a 1 kHz sine's gain is ${gain} dB, want below ${ceiling}")
    endif()
    message(STATUS "${params}: gain ${gain} dB")
endfunction()

# A readout, within `tolerance` thousandths of `want`.
function(expect_meter params key want tolerance)
    measure("${params}" json)
    string(JSON value GET "${json}" signals 0 meters ${key})
    milli("${value}" got)
    milli("${want}" wanted)
    math(EXPR off "${got} - ${wanted}")
    if(off GREATER ${tolerance} OR off LESS -${tolerance})
        message(FATAL_ERROR "${params}: ${key} reads ${value}, want ${want}")
    endif()
    message(STATUS "${params}: ${key} ${value}")
endfunction()

expect("{\"mix\":0}" 0 10)
expect("{\"mix\":0,\"output\":-6}" -6 10)
# Two stages at 1 kHz: their phase there is exactly -180 degrees.
expect_below("{\"stages\":2,\"range\":0,\"centre\":1000}" -60)
# Feedback 50%: 0.25 / |1 + 0.5 e^-jw| at w = 2 pi 1000 / 48000, less the
# level the loop adds on noise taken back ((0.5 / 0.583)^0.4): -16.08 dB.
expect("{\"stages\":2,\"range\":0,\"centre\":1000,\"feedback\":50}" -16.08 30)
# Keep below at the notch: the split's low part, half the sine, passes.
expect("{\"stages\":2,\"range\":0,\"centre\":1000,\"keep_below\":1000}" -6.02 30)
# The wet alone, held still: an all-pass.
expect("{\"range\":0,\"mix\":100}" 0 10)
# The wet alone, turning (shifted 1 Hz): the sine's level is kept.
expect_meter("{\"motion\":\"rise\",\"rate\":1,\"mix\":100}" output_level -12.0 100)
expect_meter("{\"range\":0,\"centre\":1000}" centre_now 1000 1)
