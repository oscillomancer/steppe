# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Teasel, measured through the built plugin (dsp.test), whose sine peaks at
# -12 dBFS on both channels. Dry (mix 0) it passes unchanged through the
# oversampling; a curve driven only gently, with auto gain, keeps the level;
# without auto gain the clip curve below its ceiling is the drive's gain
# alone; driving only the side leaves a centred sine clean, as does a band
# above it; output trims what leaves. The thd readout (a probe of its own)
# agrees with dsp.test's measurement of a 100 Hz sine at -18 dBFS RMS.
# Required: SANDBOX (steppe-sandbox), PLUGIN (Teasel.clap), WORK.
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
    set(level -12)
    if(ARGC GREATER 3)
        set(level ${ARGV3})
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "HOME=${home}" "USERPROFILE=${home}" "APPDATA=${home}/config"
                "LOCALAPPDATA=${home}/local" "XDG_DATA_HOME=${home}/data" "${SANDBOX}" exec
                "{\"command\":\"dsp.test\",\"args\":{\"plugin\":\"${PLUGIN}\",\"seconds\":2,\"frequency_hz\":${hz},\"level_db\":${level},\"signals\":[\"sine\"],\"patch\":{\"params\":${params}}}}"
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

expect("{\"mix\":0}" 1000 0)
expect("{\"mix\":0,\"output\":-6}" 1000 -6)
expect("{\"style\":\"soft\",\"drive\":-12}" 1000 0)
expect("{\"style\":\"clip\",\"character\":0,\"drive\":6,\"auto_gain\":false}" 1000 6)
expect("{\"style\":\"clip\",\"drive\":24,\"auto_gain\":false,\"channels\":\"side\"}" 1000 0)
expect("{\"style\":\"clip\",\"drive\":24,\"auto_gain\":false,\"band_low\":2000}" 100 0)
expect("{\"mix\":0,\"phase\":\"linear\",\"quality\":\"x8\"}" 10000 0)

# The probe's THD against the sine's, within a tenth of a percent.
function(expect_thd params)
    measure("${params}" 100 json -14.99)
    string(JSON measured GET "${json}" signals 0 thd_percent)
    string(JSON readout GET "${json}" signals 0 meters thd)
    milli("${measured}" a)
    milli("${readout}" b)
    math(EXPR off "${a} - ${b}")
    if(off GREATER 100 OR off LESS -100)
        message(FATAL_ERROR "${params}: thd reads ${readout}%, dsp.test measures ${measured}%")
    endif()
    message(STATUS "${params}: thd reads ${readout}%, measured ${measured}%")
endfunction()

expect_thd("{\"style\":\"tape\",\"drive\":6}")
expect_thd("{\"style\":\"tube\",\"drive\":12,\"bias\":30}")
