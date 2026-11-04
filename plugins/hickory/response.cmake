# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Hickory, measured through the built plugin (dsp.test), whose test signals
# are the same on both channels. At rest a sine passes unchanged; Gain sets
# its level exactly, and its minimum is silence; the side of identical
# channels is silence; the DC block takes 0.02 dB at 20 Hz.
# Required: SANDBOX (steppe-sandbox), PLUGIN (Hickory.clap), WORK.
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

function(measure params hz out)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "HOME=${home}" "USERPROFILE=${home}" "APPDATA=${home}/config"
                "LOCALAPPDATA=${home}/local" "XDG_DATA_HOME=${home}/data" "${SANDBOX}" exec
                "{\"command\":\"dsp.test\",\"args\":{\"plugin\":\"${PLUGIN}\",\"seconds\":2,\"frequency_hz\":${hz},\"signals\":[\"sine\"],\"patch\":{\"params\":${params}}}}"
                --workdir "${WORK}/workdir"
        RESULT_VARIABLE result OUTPUT_VARIABLE json ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "dsp.test failed (${result}) for ${params}:\n${json}\n${err}")
    endif()
    set(${out} "${json}" PARENT_SCOPE)
endfunction()

# The sine's gain and its peak with these settings, which must be within
# 0.01 dB of `want` (a peak under -150 dBFS when want is "silence").
function(expect params hz want)
    measure("${params}" ${hz} json)
    string(JSON gain GET "${json}" signals 0 gain_db)
    string(JSON peak GET "${json}" signals 0 peak_dbfs)
    milli("${peak}" got_peak)
    if(want STREQUAL "silence")
        if(got_peak GREATER -150000)
            message(FATAL_ERROR "${params}: a ${hz} Hz sine should be silent, peaks at ${peak} dBFS")
        endif()
    else()
        milli("${gain}" got)
        milli("${want}" wanted)
        math(EXPR off "${got} - ${wanted}")
        if(off GREATER 10 OR off LESS -10)
            message(FATAL_ERROR "${params}: a ${hz} Hz sine's gain is ${gain} dB, want ${want}")
        endif()
    endif()
    message(STATUS "${params} at ${hz} Hz: gain ${gain} dB, peak ${peak} dBFS")
endfunction()

expect("{}" 1000 0)
expect("{\"gain\":-6}" 1000 -6)
expect("{\"gain\":12.5}" 1000 12.5)
expect("{\"gain\":-60}" 1000 silence)
expect("{\"mute\":true}" 1000 silence)
expect("{\"channels\":\"side\"}" 1000 silence)
expect("{\"channels\":\"mono\",\"width\":0,\"bass_mono\":true}" 1000 0)
expect("{\"invert_left\":true,\"invert_right\":true}" 1000 0)
# The DC block's 12 dB/oct at 5 Hz: 0.02 dB down at 20 Hz.
expect("{\"dc_block\":true}" 20 -0.02)

# The 50 ms delay must retain the second half of a 100 ms input, rather
# than letting the host stop at its musical end on a reported zero tail.
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "HOME=${home}" "USERPROFILE=${home}" "APPDATA=${home}/config"
            "LOCALAPPDATA=${home}/local" "XDG_DATA_HOME=${home}/data" "${SANDBOX}" exec
            "{\"command\":\"dsp.test\",\"args\":{\"plugin\":\"${PLUGIN}\",\"seconds\":0.1,\"sample_rate\":48000,\"signals\":[\"sine\"],\"patch\":{\"params\":{\"delay\":50}},\"save\":true}}"
            --workdir "${WORK}/delay-tail"
    RESULT_VARIABLE result OUTPUT_VARIABLE json ERROR_VARIABLE err)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "delay tail: dsp.test failed (${result}):\n${json}\n${err}")
endif()
string(JSON path GET "${json}" signals 0 path)
# Stereo float32 WAV: 7200 frames need 57600 bytes of audio, before any
# header. The old truncated render has only 38400 bytes plus its header.
file(SIZE "${path}" bytes)
if(bytes LESS 57600 OR bytes GREATER 58000)
    message(FATAL_ERROR "50 ms delay lost its tail: WAV has ${bytes} bytes, expected 7200 frames")
endif()
message(STATUS "50 ms delay retains its tail: ${bytes}-byte WAV")
