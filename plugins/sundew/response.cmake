# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Sundew, measured through the built plugin (dsp.test): a steady sine settles
# at the gain its law gives. Clean with a hard knee takes 18 dB off a sine
# 24 dB over a 4:1 threshold at 44.1, 48 and 96 kHz, and 12 dB at 2:1; Range
# stops the cut; Opto, a feedback cell, gives the ratio set; Bloom lifts a
# quiet sine; below the threshold nothing changes. Lookahead reports its
# latency. Required: SANDBOX (steppe-sandbox), PLUGIN (Sundew.clap), WORK.
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

function(measure params level rate out)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "HOME=${home}" "USERPROFILE=${home}" "APPDATA=${home}/config"
                "LOCALAPPDATA=${home}/local" "XDG_DATA_HOME=${home}/data" "${SANDBOX}" exec
                "{\"command\":\"dsp.test\",\"args\":{\"plugin\":\"${PLUGIN}\",\"seconds\":2,\"sample_rate\":${rate},\"level_db\":${level},\"signals\":[\"sine\"],\"patch\":{\"params\":${params}}}}"
                --workdir "${WORK}/workdir"
        RESULT_VARIABLE result OUTPUT_VARIABLE json ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "dsp.test failed (${result}) for ${params}:\n${json}\n${err}")
    endif()
    set(${out} "${json}" PARENT_SCOPE)
endfunction()

# The gain a sine at a level settles at, within 0.2 dB of the law, as pure as it came.
function(expect params level rate want what)
    measure("${params}" ${level} ${rate} json)
    string(JSON gain GET "${json}" signals 0 gain_db)
    string(JSON thd GET "${json}" signals 0 thd_percent)
    milli("${gain}" got)
    milli("${thd}" distortion)
    math(EXPR off "${got} - ${want} * 1000")
    if(off GREATER 200 OR off LESS -200 OR distortion GREATER 1)
        message(FATAL_ERROR "${what}: ${gain} dB (want ${want}), ${thd}% THD")
    endif()
    message(STATUS "${what}: ${gain} dB, ${thd}% THD")
endfunction()

foreach(rate 44100 48000 96000)
    expect("{\"threshold\":-30,\"ratio\":4,\"knee\":0}" -6 ${rate} -18 "Clean 4:1, 24 dB over, ${rate} Hz")
endforeach()
expect("{\"threshold\":-30,\"ratio\":2,\"knee\":0}" -6 48000 -12 "Clean 2:1, 24 dB over")
expect("{\"threshold\":-40,\"ratio\":20,\"range\":6}" -6 48000 -6 "Clean held to a 6 dB range")
expect("{\"threshold\":-6,\"ratio\":4}" -24 48000 0 "Clean below the threshold")
expect("{\"character\":\"opto\",\"threshold\":-30,\"ratio\":4,\"knee\":0}" -6 48000 -18
       "Opto 4:1, 24 dB over")
expect("{\"character\":\"bloom\",\"threshold\":-20,\"ratio\":2,\"knee\":0}" -40 48000 10
       "Bloom 2:1, 20 dB under")

# Lookahead delays the audio and says so: 1, 2 and 5 ms, in samples.
foreach(run "ms1;48000;48" "ms2;48000;96" "ms5;48000;240" "ms5;96000;480")
    list(GET run 0 choice)
    list(GET run 1 rate)
    list(GET run 2 want)
    measure("{\"lookahead\":\"${choice}\"}" -24 ${rate} json)
    string(JSON latency GET "${json}" latency)
    if(NOT latency EQUAL want)
        message(FATAL_ERROR "lookahead ${choice} at ${rate} Hz reports ${latency} samples (want ${want})")
    endif()
    message(STATUS "lookahead ${choice} at ${rate} Hz: ${latency} samples")
endforeach()
