# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Umber, measured through the built plugin (dsp.test), wow and flutter off.
# Quiet sines come back at their own level at any frequency (playback undoes
# the record equalisation) but for the head bump; the drive's gain shows
# without auto gain; and the tape's third harmonic follows the datasheet it
# was fitted to: about 0.1% at reference level (a sine at -18 dBFS RMS, which
# the VU meter reads as 0) and 3% at 12.5 dB above it. The highs saturate on
# their own: a 10 kHz sine 12 dB over reference loses about 3 dB.
# Required: SANDBOX (steppe-sandbox), PLUGIN (Umber.clap), WORK.
foreach(var SANDBOX PLUGIN WORK)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "response: -D${var}=... is required")
    endif()
endforeach()
# A decimal, plain or in exponent form, as an integer number of thousandths,
# truncated (CMake's math has no decimals).
function(milli value out)
    if(NOT value MATCHES "^(-?)([0-9]+)(\\.([0-9]*))?([eE]([+-]?)0*([0-9]*))?$")
        message(FATAL_ERROR "not a number: ${value}")
    endif()
    set(sign "${CMAKE_MATCH_1}")
    set(digits "${CMAKE_MATCH_2}${CMAKE_MATCH_4}")
    string(LENGTH "${CMAKE_MATCH_4}" places)
    set(exponent "${CMAKE_MATCH_6}0${CMAKE_MATCH_7}")
    # Move the point three places right, plus the exponent.
    math(EXPR shift "${exponent} + 3 - ${places}")
    if(shift LESS 0)
        string(LENGTH "${digits}" length)
        math(EXPR length "${length} + ${shift}")
        if(length LESS_EQUAL 0)
            set(${out} 0 PARENT_SCOPE)
            return()
        endif()
        string(SUBSTRING "${digits}" 0 ${length} digits)
    elseif(shift GREATER 0)
        string(REPEAT "0" ${shift} zeros)
        string(APPEND digits "${zeros}")
    endif()
    string(REGEX MATCH "[1-9].*|0$" digits "${digits}")  # drop leading zeros
    math(EXPR m "${sign}${digits}")
    set(${out} ${m} PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK}")
set(home "${WORK}/home")
file(MAKE_DIRECTORY "${home}")

# A sine at hz and level (dBFS peak) through the plugin, wow and flutter off.
function(measure hz level params out)
    string(JSON params SET "${params}" wow 0)
    string(JSON params SET "${params}" flutter 0)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "HOME=${home}" "USERPROFILE=${home}" "APPDATA=${home}/config"
                "LOCALAPPDATA=${home}/local" "XDG_DATA_HOME=${home}/data" "${SANDBOX}" exec
                "{\"command\":\"dsp.test\",\"args\":{\"plugin\":\"${PLUGIN}\",\"seconds\":1.5,\"frequency_hz\":${hz},\"level_db\":${level},\"signals\":[\"sine\"],\"patch\":{\"params\":${params}}}}"
                --workdir "${WORK}/workdir"
        RESULT_VARIABLE result OUTPUT_VARIABLE json ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "dsp.test failed (${result}) for ${params}:\n${json}\n${err}")
    endif()
    set(${out} "${json}" PARENT_SCOPE)
endfunction()

# A value read from the result, within `tolerance` thousandths of `want`.
function(expect hz level params what want tolerance)
    measure(${hz} ${level} "${params}" json)
    if(what STREQUAL "gain")
        string(JSON value GET "${json}" signals 0 gain_db)
    elseif(what STREQUAL "thd")
        string(JSON value GET "${json}" signals 0 thd_percent)
    else()
        string(JSON value GET "${json}" signals 0 meters ${what})
    endif()
    milli("${value}" got)
    milli("${want}" wanted)
    math(EXPR off "${got} - ${wanted}")
    if(off GREATER ${tolerance} OR off LESS -${tolerance})
        message(FATAL_ERROR "${params}: ${what} of a ${hz} Hz sine at ${level} dBFS is ${value}, want ${want}")
    endif()
    message(STATUS "${params}: ${what} ${value} at ${hz} Hz, ${level} dBFS")
endfunction()

expect(1000 -35 "{}" gain 0 20)
expect(10000 -35 "{}" gain 0 50)
expect(1000 -35 "{\"output\":-6}" gain -6 20)
expect(1000 -35 "{\"drive\":6,\"auto_gain\":false}" gain 6 30)
# The head bump at 15 ips (57 Hz), and none.
expect(60 -35 "{}" gain 1.93 100)
expect(60 -35 "{\"head_bump\":0}" gain 0 50)
expect(1000 -15 "{}" vu 0 100)
expect(1000 -15 "{}" thd 0.1 50)
expect(1000 -2.5 "{}" thd 3 1000)
expect(10000 -3 "{}" gain -2.9 500)
