# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Gorse, measured through the built plugin (dsp.test). A sine that starts
# from silence is a hit with an onset and no tail: with Attack +6 dB its
# onset peaks 6 dB higher, from its first sample, at any level and at 44.1,
# 48 and 96 kHz, while the steady tone after it passes unchanged and as pure
# as it came. With Clip on, driven 24 dB over, nothing passes 0 dBFS.
# Required: SANDBOX (steppe-sandbox), PLUGIN (Gorse.clap), WORK.
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

function(measure params signals level rate out)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "HOME=${home}" "USERPROFILE=${home}" "APPDATA=${home}/config"
                "LOCALAPPDATA=${home}/local" "XDG_DATA_HOME=${home}/data" "${SANDBOX}" exec
                "{\"command\":\"dsp.test\",\"args\":{\"plugin\":\"${PLUGIN}\",\"seconds\":2,\"sample_rate\":${rate},\"level_db\":${level},\"signals\":${signals},\"patch\":{\"params\":${params}}}}"
                --workdir "${WORK}/workdir"
        RESULT_VARIABLE result OUTPUT_VARIABLE json ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "dsp.test failed (${result}) for ${params}:\n${json}\n${err}")
    endif()
    set(${out} "${json}" PARENT_SCOPE)
endfunction()

# The onset raised 6 dB, the steady tone untouched: at -12 dBFS at three
# rates, and 20 dB quieter at 48 kHz.
foreach(run "-12;44100" "-12;48000" "-12;96000" "-32;48000")
    list(GET run 0 level)
    list(GET run 1 rate)
    measure("{\"attack\":6}" "[\"sine\"]" ${level} ${rate} json)
    string(JSON peak GET "${json}" signals 0 peak_dbfs)
    string(JSON gain GET "${json}" signals 0 gain_db)
    string(JSON thd GET "${json}" signals 0 thd_percent)
    milli("${peak}" got)
    math(EXPR want "(${level} + 6) * 1000")
    math(EXPR off "${got} - ${want}")
    milli("${gain}" steady)
    milli("${thd}" distortion)
    if(off GREATER 50 OR off LESS -50 OR steady GREATER 10 OR steady LESS -10 OR distortion GREATER 1)
        message(FATAL_ERROR "a sine at ${level} dBFS, ${rate} Hz, Attack +6 dB: onset peak ${peak} dBFS "
                            "(want ${level}+6), steady ${gain} dB, ${thd}% THD")
    endif()
    message(STATUS "a sine at ${level} dBFS, ${rate} Hz: onset ${peak} dBFS, steady ${gain} dB, ${thd}% THD")
endforeach()

# Clip: driven 24 dB over full scale, every signal stops at 0 dBFS.
measure("{\"attack\":12,\"output\":12,\"clip\":true}" "[\"sine\",\"sweep\",\"noise\",\"impulse\"]" 0 48000
        json)
foreach(i RANGE 3)
    string(JSON name GET "${json}" signals ${i} signal)
    string(JSON peak GET "${json}" signals ${i} peak_dbfs)
    milli("${peak}" got)
    if(got GREATER 0)
        message(FATAL_ERROR "${name} driven 24 dB into the clip peaks at ${peak} dBFS")
    endif()
    message(STATUS "${name} driven 24 dB into the clip peaks at ${peak} dBFS")
endforeach()
