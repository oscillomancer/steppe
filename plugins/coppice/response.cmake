# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Coppice, measured through the built plugin (dsp.test): its default plays
# its factory keys in tune from the root, a loop sustains a held note where
# the same note without it has ended, a one-shot plays on after its key,
# the keys past the last slice are silent, and a factory sample plays as
# the same file does from disk.
# Required: SANDBOX (steppe-sandbox), PLUGIN (Coppice.clap), WORK.
foreach(var SANDBOX PLUGIN WORK)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "response: -D${var}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
set(home "${WORK}/home")
file(MAKE_DIRECTORY "${home}")

# dsp.test of the plugin with `args` (members after "plugin"); its result in
# `out`. What it finds wrong fails the test unless a third argument says it
# is expected (an A4 that is not 440 Hz, a key with no slice).
function(measure args out)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "HOME=${home}" "USERPROFILE=${home}" "APPDATA=${home}/config"
                "LOCALAPPDATA=${home}/local" "XDG_DATA_HOME=${home}/data" "${SANDBOX}" exec
                "{\"command\":\"dsp.test\",\"args\":{\"plugin\":\"${PLUGIN}\",${args}}}"
                --workdir "${WORK}/workdir"
        RESULT_VARIABLE result OUTPUT_VARIABLE json ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "dsp.test failed (${result}) for ${args}:\n${json}\n${err}")
    endif()
    string(JSON problems ERROR_VARIABLE none GET "${json}" problems)
    if(ARGC EQUAL 2 AND NOT none AND NOT problems STREQUAL "" AND NOT problems STREQUAL "[]")
        message(FATAL_ERROR "${args}: ${problems}")
    endif()
    set(${out} "${json}" PARENT_SCOPE)
endfunction()

# The A4 note with these parameters must sound within 5 cents of `hz`.
function(expect params hz)
    measure("\"seconds\":2,\"signals\":[\"note\"],\"patch\":{\"params\":${params}}" json off-pitch)
    string(JSON pitch GET "${json}" signals 0 pitch_hz)
    string(REGEX REPLACE "\\..*" "" whole "${pitch}")
    math(EXPR low "${hz} * 997 / 1000")
    math(EXPR high "${hz} * 1003 / 1000")
    if(whole LESS low OR whole GREATER high)
        message(FATAL_ERROR "${params}: the A4 note sounds at ${pitch} Hz, want ${hz}")
    endif()
    message(STATUS "${params}: ${pitch} Hz")
endfunction()

expect("{}" 440)
expect("{\"sample.transpose\":12}" 880)
expect("{\"sample.root\":69}" 261)
expect("{\"voice.oversampling\":\"x1\"}" 440)
expect("{\"voice.oversampling\":\"x4\",\"filter.type\":\"ladder\"}" 440)
expect("{\"sample.keytrack\":false}" 261)

# How long the sound goes on after the key of `notes` is up.
function(release patch notes out)
    measure("\"seconds\":4,\"notes\":${notes},\"patch\":${patch}" json)
    string(JSON ms GET "${json}" signals 0 release_ms)
    string(REGEX REPLACE "\\..*" "" ms "${ms}")
    set(${out} ${ms} PARENT_SCOPE)
endfunction()

# The pad played at A4 runs out after 1.2 s: held for 2, it has ended by the
# key's release unless it loops, and then it rings for the release, 100 ms.
set(held "[{\"key\":69,\"velocity\":0.8,\"start_s\":0,\"length_s\":2}]")
set(pad "\"properties\":{\"sample\":\"factory:pad.flac\"}")
release("{${pad},\"params\":{\"sample.root\":48}}" "${held}" ended)
release("{${pad},\"params\":{\"sample.root\":48,\"loop.mode\":\"forward\"}}" "${held}" forward)
release("{${pad},\"params\":{\"sample.root\":48,\"loop.mode\":\"ping_pong\"}}" "${held}" pingpong)
if(NOT ended EQUAL 0 OR forward LESS 90 OR forward GREATER 110 OR pingpong LESS 90 OR pingpong GREATER 110)
    message(FATAL_ERROR "held past the pad's end: ${ended} ms after the key without a loop "
                        "(want 0), ${forward} forward and ${pingpong} ping-pong (want 100)")
endif()
message(STATUS "a held pad: ${ended} ms after its key, looped ${forward} and ${pingpong} ms")

# A one-shot plays the keys at A4, 2.4 s, whatever the key does.
release("{\"params\":{\"sample.mode\":\"one_shot\"}}"
        "[{\"key\":69,\"velocity\":0.8,\"start_s\":0,\"length_s\":0.1}]" oneshot)
if(oneshot LESS 2000)
    message(FATAL_ERROR "a one-shot of the keys at A4 plays ${oneshot} ms after its key, want 2300")
endif()
message(STATUS "a one-shot: ${oneshot} ms after its key")

# Sixteen slices of the break from C2 (36): the last on 51, nothing on 52.
foreach(key 36 51 52)
    measure("\"seconds\":1,\"notes\":[{\"key\":${key},\"velocity\":0.8,\"start_s\":0,\"length_s\":0.2}],\"patch\":{\"properties\":{\"sample\":\"factory:break.flac\"},\"params\":{\"sample.mode\":\"slice\",\"slice.by\":\"grid\"}}" json silent)
    string(JSON peak GET "${json}" signals 0 peak_dbfs)
    string(REGEX REPLACE "\\..*" "" peak "${peak}")
    if((key EQUAL 52 AND peak GREATER -150) OR (NOT key EQUAL 52 AND peak LESS -60))
        message(FATAL_ERROR "the break's slice on key ${key} peaks at ${peak} dBFS")
    endif()
endforeach()
message(STATUS "sixteen slices: keys 36 to 51 play, 52 is silent")

# factory:keys.flac plays exactly as samples/keys.flac does from disk.
foreach(source "factory:keys.flac" "${CMAKE_CURRENT_LIST_DIR}/samples/keys.flac")
    measure("\"seconds\":2,\"signals\":[\"notes\"],\"patch\":{\"properties\":{\"sample\":\"${source}\"}}" json)
    string(JSON peak GET "${json}" signals 0 peak_dbfs)
    string(JSON rms GET "${json}" signals 0 rms_dbfs)
    list(APPEND levels "${peak}/${rms}")
endforeach()
list(GET levels 0 a)
list(GET levels 1 b)
if(NOT a STREQUAL b)
    message(FATAL_ERROR "factory:keys.flac plays at ${a} dBFS (peak/RMS), the file at ${b}")
endif()
message(STATUS "the factory keys and the file: ${a} dBFS both")
