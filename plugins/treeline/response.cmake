# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Treeline's ceiling, measured through the built plugin (dsp.test): driven
# 12 dB into a -3 dB ceiling at 44.1, 48 and 96 kHz, a sine, a sweep, noise
# and an impulse each peak at the ceiling and not above it; below the
# ceiling it changes nothing. Required: SANDBOX (steppe-sandbox), PLUGIN (Treeline.clap), WORK.
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
                "{\"command\":\"dsp.test\",\"args\":{\"plugin\":\"${PLUGIN}\",\"seconds\":2,\"sample_rate\":${rate},\"level_db\":${level},\"signals\":[\"sine\",\"sweep\",\"noise\",\"impulse\"],\"patch\":{\"params\":${params}}}}"
                --workdir "${WORK}/workdir"
        RESULT_VARIABLE result OUTPUT_VARIABLE json ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "dsp.test failed (${result}) for ${params}:\n${json}\n${err}")
    endif()
    set(${out} "${json}" PARENT_SCOPE)
endfunction()

# Driven into the ceiling: every signal reaches it (within 0.3 dB) and none passes it.
foreach(rate 44100 48000 96000)
    measure("{\"gain\":12,\"ceiling\":-3}" -1 ${rate} json)
    foreach(i RANGE 3)
        string(JSON name GET "${json}" signals ${i} signal)
        string(JSON peak GET "${json}" signals ${i} peak_dbfs)
        milli("${peak}" got)
        if(got GREATER -3000 OR got LESS -3300)
            message(FATAL_ERROR "${name} at ${rate} Hz driven 12 dB into a -3 dB ceiling peaks at ${peak} dBFS")
        endif()
        message(STATUS "${name} at ${rate} Hz driven into a -3 dB ceiling peaks at ${peak} dBFS")
    endforeach()
endforeach()

# Below the ceiling: a sine passes at its own level and as pure as it came.
measure("{\"gain\":0,\"ceiling\":-1}" -12 48000 json)
string(JSON gain GET "${json}" signals 0 gain_db)
string(JSON thd GET "${json}" signals 0 thd_percent)
milli("${gain}" got)
milli("${thd}" distortion)
if(got GREATER 10 OR got LESS -10 OR distortion GREATER 1)
    message(FATAL_ERROR "a sine under the ceiling changes: ${gain} dB, ${thd}% THD")
endif()
message(STATUS "a sine under the ceiling: ${gain} dB, ${thd}% THD")
