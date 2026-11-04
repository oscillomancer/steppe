# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Bracken, measured through the built plugin (dsp.test), whose sine peaks at
# -12 dBFS on both channels. Dry (mix 0) it passes unchanged; a single echo
# a millisecond late, alone, is the sine at its own level; ducking takes 12
# dB off the echoes while the sine plays; output trims what leaves. The
# echo time and decay readouts agree with the time and feedback as set.
# Required: SANDBOX (steppe-sandbox), PLUGIN (Bracken.clap), WORK.
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

function(measure params out)
    set(save false)
    if(ARGC GREATER 2)
        set(save true)
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "HOME=${home}" "USERPROFILE=${home}" "APPDATA=${home}/config"
                "LOCALAPPDATA=${home}/local" "XDG_DATA_HOME=${home}/data" "${SANDBOX}" exec
                "{\"command\":\"dsp.test\",\"args\":{\"plugin\":\"${PLUGIN}\",\"seconds\":2,\"sample_rate\":48000,\"frequency_hz\":1000,\"level_db\":-12,\"signals\":[\"sine\"],\"save\":${save},\"patch\":{\"params\":${params}}}}"
                --workdir "${WORK}/workdir"
        RESULT_VARIABLE result OUTPUT_VARIABLE json ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "dsp.test failed (${result}) for ${params}:\n${json}\n${err}")
    endif()
    set(${out} "${json}" PARENT_SCOPE)
endfunction()

# Render a continuous two-second source so gaps between the 1.5 s repeats
# cannot trigger silence detection. The renderer stops at clap.tail; check
# that boundary directly as well as the separately measured decay readout.
measure("{\"mix\":0}" source true)
string(JSON source GET "${source}" signals 0 path)
file(TO_CMAKE_PATH "${source}" source)

function(expect_tail params want)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "HOME=${home}" "USERPROFILE=${home}" "APPDATA=${home}/config"
                "LOCALAPPDATA=${home}/local" "XDG_DATA_HOME=${home}/data" "${SANDBOX}" exec
                "{\"batch\":{\"steps\":[{\"command\":\"plugin.load\",\"args\":{\"plugin\":\"${PLUGIN}\"}},{\"command\":\"patch.merge\",\"args\":{\"instance\":\"in_1\",\"patch\":{\"params\":${params}}}},{\"command\":\"render\",\"args\":{\"audio_file\":\"${source}\",\"tail_s\":60}}]}}"
                --workdir "${WORK}/tail"
        RESULT_VARIABLE result OUTPUT_VARIABLE json ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "tail render failed (${result}) for ${params}:\n${json}\n${err}")
    endif()
    string(JSON value GET "${json}" results 2 tail_s)
    string(JSON capped GET "${json}" results 2 tail_capped)
    milli("${value}" got)
    milli("${want}" wanted)
    math(EXPR off "${got} - ${wanted}")
    if(capped OR off GREATER 2 OR off LESS -2)
        message(FATAL_ERROR "${params}: rendered tail ${value} s (capped=${capped}), want ${want}")
    endif()
    message(STATUS "${params}: rendered tail ${value} s")
endfunction()

# The sine's gain with these settings, which must be within 0.01 dB of `want`.
function(expect params want)
    measure("${params}" json)
    string(JSON gain GET "${json}" signals 0 gain_db)
    milli("${gain}" got)
    milli("${want}" wanted)
    math(EXPR off "${got} - ${wanted}")
    if(off GREATER 10 OR off LESS -10)
        message(FATAL_ERROR "${params}: a 1 kHz sine's gain is ${gain} dB, want ${want}")
    endif()
    message(STATUS "${params}: gain ${gain} dB")
endfunction()

expect("{\"mix\":0}" 0)
expect("{\"mix\":0,\"output\":-6}" -6)
expect("{\"sync\":\"free\",\"time\":1,\"feedback\":0,\"mix\":100}" 0)
expect("{\"sync\":\"free\",\"time\":1,\"feedback\":0,\"mix\":100,\"duck\":12}" -12)

# A readout, within `tolerance` thousandths of `want`.
function(expect_meter params key want tolerance)
    measure("${params}" json)
    string(JSON value GET "${json}" signals 0 meters ${key})
    milli("${value}" got)
    milli("${want}" wanted)
    math(EXPR off "${got} - ${wanted}")
    if(off GREATER ${tolerance} OR off LESS -${tolerance})
        message(FATAL_ERROR "${params}: ${key} reads ${value}, want ${want}")
    endif()
    message(STATUS "${params}: ${key} ${value}")
endfunction()

# A dotted eighth at 120 BPM (the tempo when the host sends none).
expect_meter("{\"sync\":\"n8d\"}" echo_time 375 1)
# Each echo 6.02 dB below the last, every 250 ms: 60 dB in 2.491 s.
expect_meter(
    "{\"sync\":\"free\",\"time\":250,\"feedback\":50,\"low_cut\":20,\"high_cut\":20000,\"age\":0}"
    decay 2491.4 1000)

# Regression: the nominal 1 s echo decays in 24.012 s, but offset +50%
# makes the right-hand loop last 36.018 s. This failed on the old binary,
# which used the nominal time for both decay and the host's tail.
set(offset_patch
    "{\"sync\":\"free\",\"time\":1000,\"feedback\":75,\"offset\":50,\"low_cut\":20,\"high_cut\":20000,\"age\":0,\"mix\":100}")
expect_tail("${offset_patch}" 50.3735)
expect_meter("${offset_patch}" decay 36017.65 10)

# A shorter right side cannot shorten stereo's left side. Ping-pong offsets
# only B, while mono ignores offset. Reverse changes the tail, not loop loss.
string(JSON params SET "${offset_patch}" offset -50)
expect_meter("${params}" decay 24011.77 10)
string(JSON params SET "${offset_patch}" routing "\"ping_pong\"")
expect_meter("${params}" decay 30014.71 10)
string(JSON params SET "${params}" groove 50)
expect_meter("${params}" decay 27013.24 10)
string(JSON params SET "${offset_patch}" routing "\"mono\"")
expect_meter("${params}" decay 24011.77 10)
string(JSON params SET "${offset_patch}" reverse true)
expect_tail("${params}" 52.6235)
expect_meter("${params}" decay 36017.65 10)

# Clamp each half before applying and clamping its offset: A=5 s, B=2.5 s,
# then right A=5 s, B=3.75 s. Its mean interval is 4.375 s, not 7.5 s.
string(JSON params SET "${offset_patch}" time 5000)
string(JSON params SET "${params}" groove 50)
expect_meter("${params}" decay 105051.49 10)
string(JSON params SET "${params}" routing "\"mono\"")
expect_meter("${params}" decay 90044.13 10)

# The minimum clamp also matters: A=1.5 ms, B=1 ms on the left; offset
# -50% clamps the right back to 1 ms each, so the left's 1.25 ms mean wins.
string(JSON params SET "${offset_patch}" time 1)
string(JSON params SET "${params}" groove 50)
string(JSON params SET "${params}" offset -50)
expect_meter("${params}" decay 30.0147 2)

# Each analog half has its own clock and loss. Here the longer right side
# loses more at 1 kHz (9.184 s decay), so the left side's 11.708 s wins.
string(JSON params SET "${offset_patch}" medium "\"analog\"")
string(JSON params SET "${params}" age 30)
string(JSON params SET "${params}" groove 50)
expect_meter("${params}" decay 11708.45 10)
