# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Ghyll, measured through the built plugin (dsp.test), whose sine peaks at
# -12 dBFS on both channels. Dry (mix 0) it passes unchanged and output trims
# what leaves. The readouts follow the room as set: the distance between two
# places, and a concrete cube's reverberation time by Eyring's formula.
# Required: SANDBOX (steppe-sandbox), PLUGIN (Ghyll.clap), WORK.
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

# The sine's gain with these settings, which must be within 0.01 dB of `want`.
function(expect params want)
    measure("${params}" json)
    string(JSON gain GET "${json}" signals 0 gain_db)
    milli("${gain}" got)
    milli("${want}" wanted)
    math(EXPR off "${got} - ${wanted}")
    if(off GREATER 10 OR off LESS -10)
        message(FATAL_ERROR "${params}: a 1 kHz sine's gain is ${gain} dB, want ${want}")
    endif()
    message(STATUS "${params}: gain ${gain} dB")
endfunction()

expect("{\"mix\":0}" 0)
expect("{\"mix\":0,\"output\":-6}" -6)

# Only the stone floor reflects: a single path from a source facing the
# listener. The 10 m horizontal path has a 3.2 m image height difference;
# its source gain is 0.7 + 0.3 * 10 / sqrt(110.24), not the backwards lobe.
# Including distance, absorption, air and the three-band split gives
# -0.704 dB at 1 kHz (the backwards source gives -8.233 dB).
expect("{\"room_width\":20,\"room_depth\":20,\"room_height\":10,\"walls\":\"open\",\"floor\":\"stone\",\"ceiling\":\"open\",\"source_x\":50,\"source_y\":25,\"listener_x\":50,\"listener_y\":75,\"mic\":\"mono\",\"scatter\":0,\"mix\":100,\"late\":-30}"
    -0.704)

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

# 10 m apart on the plan, the listener's ear 0.2 m above the source.
expect_meter("{\"room_width\":20,\"room_depth\":20,\"room_height\":10,\"source_x\":50,\"source_y\":25,\"listener_x\":50,\"listener_y\":75}"
    distance 10.002 2)
# Coincident places still use their actual separation, floored at 0.5 m;
# choosing a fallback facing must not invent one metre of horizontal distance.
expect_meter("{\"source_x\":50,\"source_y\":50,\"listener_x\":50,\"listener_y\":50}"
    distance 0.5 1)
# A 10 m concrete cube: V 1000 m3, S 600 m2, absorbing 4% at 500 Hz and 5% at
# 1 kHz, the air 2.73 and 4.66 dB/km; the mean of the two octaves' RT60s.
expect_meter(
    "{\"room_width\":10,\"room_depth\":10,\"room_height\":10,\"walls\":\"concrete\",\"floor\":\"concrete\",\"ceiling\":\"concrete\"}"
    rt60 5276.2 1000)
