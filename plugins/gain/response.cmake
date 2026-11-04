# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Gain, measured through the built plugin (dsp.test): a sine's level changes
# by exactly the gain, across the whole range, and a jump of the whole range
# glides instead of stepping. (Block-size invariance under automation is
# plugin.gain.blocks'.)
# Required: SANDBOX (steppe-sandbox), PLUGIN (Gain.clap), WORK.
foreach(var SANDBOX PLUGIN WORK)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "response: -D${var}=... is required")
    endif()
endforeach()
# A decimal as an integer number of millionths (CMake's math has no decimals).
function(micro value out)
    if(value MATCHES "e")
        set(${out} 0 PARENT_SCOPE)  # a tiny value in exponent form
        return()
    endif()
    if(NOT value MATCHES "^(-?)([0-9]+)(\\.([0-9]*))?$")
        message(FATAL_ERROR "not a number: ${value}")
    endif()
    set(sign "${CMAKE_MATCH_1}")
    set(whole "${CMAKE_MATCH_2}")
    string(SUBSTRING "${CMAKE_MATCH_4}000000" 0 6 fraction)
    math(EXPR m "${whole} * 1000000 + 1${fraction} - 1000000")
    if(sign STREQUAL "-")
        math(EXPR m "-${m}")
    endif()
    set(${out} ${m} PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK}")
set(home "${WORK}/home")
file(MAKE_DIRECTORY "${home}")

# dsp.test's arguments besides the plugin (a JSON fragment), its result in `out`.
function(measure args out)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "HOME=${home}" "USERPROFILE=${home}" "APPDATA=${home}/config"
                "LOCALAPPDATA=${home}/local" "XDG_DATA_HOME=${home}/data" "${SANDBOX}" exec
                "{\"command\":\"dsp.test\",\"args\":{\"plugin\":\"${PLUGIN}\",\"signals\":[\"sine\"],${args}}}"
                --workdir "${WORK}/workdir"
        RESULT_VARIABLE result OUTPUT_VARIABLE json ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "dsp.test failed (${result}) for ${args}:\n${json}\n${err}")
    endif()
    set(${out} "${json}" PARENT_SCOPE)
endfunction()

# A sine's gain at this setting, exactly as dsp.test reports it (to the
# hundredth of a dB; CMake reads 6.02 as 6.0199999999999996). The sine is
# quiet enough that +32 dB stays under full scale.
function(expect_gain db)
    measure("\"level_db\":-36,\"patch\":{\"params\":{\"gain\":${db}}}" json)
    string(JSON gain GET "${json}" signals 0 gain_db)
    micro("${gain}" got)
    micro("${db}" wanted)
    math(EXPR off "${got} - ${wanted}")
    if(off GREATER 10 OR off LESS -10)
        message(FATAL_ERROR "gain ${db}: a sine's gain is ${gain} dB")
    endif()
    message(STATUS "gain ${db}: a sine's gain is ${gain} dB")
endfunction()

expect_gain(0)
expect_gain(6)  # twice the amplitude, near enough (6.02 dB is exactly twice)
expect_gain(6.02)
expect_gain(-12.5)
expect_gain(32)
expect_gain(-32)

# The whole range at once, -32 to +32 dB, on a 50 Hz sine at its peak
# (0.505 s): sent straight through, that is a step of about 0.6 between two
# samples. Glided, the largest step is the sine's own at +32 dB, 0.0041.
measure("\"level_db\":-36,\"frequency_hz\":50,\"patch\":{\"params\":{\"gain\":-32}},\"automation\":[{\"param\":\"gain\",\"at_s\":0.505,\"value\":32}]" json)
string(JSON step GET "${json}" signals 0 automation max_step)
micro("${step}" got)
if(got GREATER 5000)
    message(FATAL_ERROR "a jump of 64 dB steps the output by ${step}: it should glide")
endif()
message(STATUS "a jump of 64 dB: the largest step is ${step}")
# And it lands: the peak is the sine's at +32 dB.
string(JSON peak GET "${json}" signals 0 peak_dbfs)
micro("${peak}" got)
if(got GREATER -3999990 OR got LESS -4000010)
    message(FATAL_ERROR "after a jump to +32 dB a -36 dBFS sine peaks at ${peak} dBFS, want -4")
endif()
message(STATUS "its peak after the jump: ${peak} dBFS")
