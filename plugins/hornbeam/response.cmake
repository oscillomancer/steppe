# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Hornbeam, measured through the built plugin (dsp.test). The cabinet alone
# is linear at low level and its response is the model's (the values its
# editor draws: a closed 4x12 of vintage speakers, a dynamic mic 2.5 cm off
# the cone at 30%); the mix's dry side is the input exactly; the gain pot
# drives the preamp from clean to distorted.
# Required: SANDBOX (steppe-sandbox), PLUGIN (Hornbeam.clap), WORK.
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

# A sine at hz and level (dBFS peak) through the plugin.
function(measure hz level params out)
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

function(read json what out)
    if(what STREQUAL "gain")
        string(JSON value GET "${json}" signals 0 gain_db)
    elseif(what STREQUAL "thd")
        string(JSON value GET "${json}" signals 0 thd_percent)
    else()
        string(JSON value GET "${json}" signals 0 meters ${what})
    endif()
    set(${out} "${value}" PARENT_SCOPE)
endfunction()

# A value read from the result, within `tolerance` thousandths of `want`.
function(expect hz level params what want tolerance)
    measure(${hz} ${level} "${params}" json)
    read("${json}" ${what} value)
    milli("${value}" got)
    milli("${want}" wanted)
    math(EXPR off "${got} - ${wanted}")
    if(off GREATER ${tolerance} OR off LESS -${tolerance})
        message(FATAL_ERROR "${params}: ${what} of a ${hz} Hz sine at ${level} dBFS is ${value}, want ${want}")
    endif()
    message(STATUS "${params}: ${what} ${value} at ${hz} Hz, ${level} dBFS")
endfunction()

# A value below (or with `above`, over) a bound.
function(bound hz level params what side limit)
    measure(${hz} ${level} "${params}" json)
    read("${json}" ${what} value)
    milli("${value}" got)
    milli("${limit}" edge)
    if((side STREQUAL "below" AND NOT got LESS edge) OR (side STREQUAL "above" AND NOT got GREATER edge))
        message(FATAL_ERROR "${params}: ${what} of a ${hz} Hz sine at ${level} dBFS is ${value}, want ${side} ${limit}")
    endif()
    message(STATUS "${params}: ${what} ${value} at ${hz} Hz, ${level} dBFS (${side} ${limit})")
endfunction()

set(cab "{\"chain\":\"cab\",\"cabinet\":\"closed4x12\",\"speaker\":\"vintage\",\"mic\":\"dynamic\",\"position\":30,\"distance\":0.025,\"room\":0}")
expect(100 -30 "${cab}" gain 0.64 50)
expect(1000 -30 "${cab}" gain -1.19 50)
expect(3000 -30 "${cab}" gain -6.78 50)
expect(8000 -30 "${cab}" gain -23.21 100)
bound(1000 -30 "${cab}" thd below 0.01)
# The dry side of the mix is the input, delayed by the latency only.
expect(1000 -12 "{\"mix\":0}" gain 0 5)
bound(1000 -12 "{\"mix\":0}" thd below 0.001)
# The gain pot: crunch nearly clean at a quarter, distorted at three quarters.
bound(200 -15 "{\"voice\":\"crunch\",\"gain\":25,\"chain\":\"amp\",\"master\":30}" distortion below 5)
bound(200 -15 "{\"voice\":\"crunch\",\"gain\":75,\"chain\":\"amp\",\"master\":30}" distortion above 15)
