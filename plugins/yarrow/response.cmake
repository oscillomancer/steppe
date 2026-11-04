# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Yarrow's filters, measured through the built plugin (dsp.test's impulse
# response at octave centres): a bell lands on its gain, a bell near the top
# octave is not cramped, and a cut is -3 dB at its corner and falls at its
# slope. Required: SANDBOX (steppe-sandbox), PLUGIN (Yarrow.clap), WORK.
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

# Each case: a patch's params, then hz=expected dB pairs, within 0.1 dB.
set(cases
    "{\"band3.enabled\":true,\"band3.freq\":1000,\"band3.gain\":6,\"band3.q\":1}|1000=6.0|500=1.89"
    "{\"band3.enabled\":true,\"band3.freq\":16000,\"band3.gain\":-12,\"band3.q\":2}|16000=-12.0"
    "{\"band1.enabled\":true,\"band1.freq\":125}|125=-3.0|62.5=-24.2|250=0.0"
    "{\"band7.enabled\":true,\"band7.freq\":4000,\"band7.gain\":-6}|4000=-3.0|16000=-6.0")
foreach(case IN LISTS cases)
    string(REPLACE "|" ";" parts "${case}")
    list(POP_FRONT parts params)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "HOME=${home}" "USERPROFILE=${home}" "APPDATA=${home}/config"
                "LOCALAPPDATA=${home}/local" "XDG_DATA_HOME=${home}/data" "${SANDBOX}" exec
                "{\"command\":\"dsp.test\",\"args\":{\"plugin\":\"${PLUGIN}\",\"signals\":[\"impulse\"],\"patch\":{\"params\":${params}}}}"
                --workdir "${WORK}/workdir"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "dsp.test failed (${result}) for ${params}:\n${out}\n${err}")
    endif()
    string(JSON response GET "${out}" signals 0 response_db)
    string(JSON count LENGTH "${response}")
    math(EXPR last "${count} - 1")
    foreach(pair IN LISTS parts)
        string(REPLACE "=" ";" pair "${pair}")
        list(GET pair 0 hz)
        list(GET pair 1 want)
        set(found "")
        foreach(i RANGE ${last})
            string(JSON at GET "${response}" ${i} hz)
            if(at EQUAL hz)
                string(JSON found GET "${response}" ${i} db)
            endif()
        endforeach()
        if(found STREQUAL "")
            message(FATAL_ERROR "no response at ${hz} Hz for ${params}")
        endif()
        milli("${found}" got)
        milli("${want}" expected)
        math(EXPR diff "${got} - ${expected}")
        if(diff GREATER 100 OR diff LESS -100)
            message(FATAL_ERROR "${params}: ${hz} Hz reads ${found} dB, expected ${want} (within 0.1 dB)")
        endif()
        message(STATUS "${params}: ${hz} Hz reads ${found} dB")
    endforeach()
endforeach()
