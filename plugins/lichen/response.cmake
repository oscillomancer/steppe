# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Lichen, measured through the built plugin (dsp.test): a 1 kHz sine peaking
# at -12 dBFS on both channels for 2 s passes unchanged, and the readings
# are what the standards and the definitions give for it. (host-smoke checks
# that every sample passes exactly, as a transparent product.)
# Required: SANDBOX (steppe-sandbox), PLUGIN (Lichen.clap), WORK.
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

execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "HOME=${home}" "USERPROFILE=${home}" "APPDATA=${home}/config"
            "LOCALAPPDATA=${home}/local" "XDG_DATA_HOME=${home}/data" "${SANDBOX}" exec
            "{\"command\":\"dsp.test\",\"args\":{\"plugin\":\"${PLUGIN}\",\"seconds\":2,\"frequency_hz\":1000,\"level_db\":-12,\"signals\":[\"sine\"]}}"
            --workdir "${WORK}/workdir"
    RESULT_VARIABLE result OUTPUT_VARIABLE json ERROR_VARIABLE err)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "dsp.test failed (${result}):\n${json}\n${err}")
endif()

# A value from the result, within `tolerance` thousandths of `want`.
function(expect what value want tolerance)
    milli("${value}" got)
    milli("${want}" wanted)
    math(EXPR off "${got} - ${wanted}")
    if(off GREATER ${tolerance} OR off LESS -${tolerance})
        message(FATAL_ERROR "${what} reads ${value}, want ${want}")
    endif()
    message(STATUS "${what}: ${value}")
endfunction()
function(expect_meter key want tolerance)
    string(JSON value GET "${json}" signals 0 meters ${key})
    expect(${key} "${value}" ${want} ${tolerance})
endfunction()

string(JSON gain GET "${json}" signals 0 gain_db)
expect(gain_db "${gain}" 0 0)
# BS.1770: a 1 kHz sine at -12 dBFS in both channels reads -12 LUFS, less
# K-weighting's 0.007 dB at 1 kHz.
expect_meter(integrated -11.993 20)
expect_meter(momentary -11.993 20)
expect_meter(true_peak -12 5)
expect_meter(sample_peak -12 5)
expect_meter(crest 3.010 5)
expect_meter(correlation 1 1)
expect_meter(width 0 1)
expect_meter(mono_loss 0 5)
expect_meter(mid 0 50)
expect_meter(measured 2000 1)
string(JSON key GET "${json}" signals 0 meters key)
if(NOT key STREQUAL "none")
    message(FATAL_ERROR "a lone sine reads as the key ${key}, want none")
endif()
