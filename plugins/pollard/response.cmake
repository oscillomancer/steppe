# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Pollard, measured through the built plugin (dsp.test): its default kit
# sounds on the keys from C2 to D#3 and nowhere else, a pad's tune moves its
# sound by exactly that much, the closed hat chokes the open one, a gated
# pad ends its release after its key, and a pad on an extra output leaves
# the main one.
# Required: SANDBOX (steppe-sandbox), PLUGIN (Pollard.clap), WORK.
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
# is expected (an A4 that is not 440 Hz, a key with no pad).
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

# One hit of `key` with the patch `patch`, held `held` seconds: its peak (dB,
# whole) and how long it sounds after its key (ms, whole).
function(hit key held patch peak release)
    measure("\"seconds\":3,\"notes\":[{\"key\":${key},\"velocity\":0.8,\"start_s\":0,\"length_s\":${held}}],\"patch\":${patch}" json silent)
    string(JSON p GET "${json}" signals 0 peak_dbfs)
    string(JSON r GET "${json}" signals 0 release_ms)
    string(REGEX REPLACE "\\..*" "" p "${p}")
    string(REGEX REPLACE "\\..*" "" r "${r}")
    set(${peak} ${p} PARENT_SCOPE)
    set(${release} ${r} PARENT_SCOPE)
endfunction()

# The default kit: a sound on each pad's key, 36 to 51, none just outside.
foreach(key 35 36 38 42 46 49 51 52)
    hit(${key} 0.1 "{}" peak release)
    if((key EQUAL 35 OR key EQUAL 52) AND peak GREATER -150)
        message(FATAL_ERROR "key ${key}, past the pads, peaks at ${peak} dBFS")
    elseif(NOT key EQUAL 35 AND NOT key EQUAL 52 AND peak LESS -40)
        message(FATAL_ERROR "the default kit's pad on key ${key} peaks at ${peak} dBFS")
    endif()
endforeach()
message(STATUS "the default kit: keys 36 to 51 play, 35 and 52 are silent")

# Tune: Coppice's keys sample (C4) on pad 1, moved to A4 so the note signal
# measures it, sounds at its own pitch, an octave up, and 7.5 semitones up.
set(keys "${CMAKE_CURRENT_LIST_DIR}/../coppice/samples/keys.flac")
foreach(pair "0;261" "12;523" "7.5;403")
    list(GET pair 0 tune)
    list(GET pair 1 hz)
    measure("\"seconds\":2,\"signals\":[\"note\"],\"patch\":{\"properties\":{\"pad1\":\"${keys}\"},\"params\":{\"pads.key\":69,\"pad1.tune\":${tune}}}" json off-pitch)
    string(JSON pitch GET "${json}" signals 0 pitch_hz)
    string(REGEX REPLACE "\\..*" "" whole "${pitch}")
    math(EXPR low "${hz} * 997 / 1000")
    math(EXPR high "${hz} * 1003 / 1000")
    if(whole LESS low OR whole GREATER high)
        message(FATAL_ERROR "pad 1 tuned ${tune} st sounds at ${pitch} Hz, want ${hz}")
    endif()
    message(STATUS "pad 1 tuned ${tune} st: ${pitch} Hz")
endforeach()

# Choke: the open hat, then the closed hat 100 ms on. In their group the open
# hat stops; out of it, it rings on long after both keys.
set(hats "\"notes\":[{\"key\":46,\"velocity\":0.8,\"start_s\":0,\"length_s\":0.05},{\"key\":42,\"velocity\":0.8,\"start_s\":0.1,\"length_s\":0.05}]")
foreach(group group1 off)
    measure("\"seconds\":2,${hats},\"patch\":{\"params\":{\"pad11.choke\":\"${group}\"}}" json)
    string(JSON ms GET "${json}" signals 0 release_ms)
    string(REGEX REPLACE "\\..*" "" ms "${ms}")
    set(rings_${group} ${ms})
endforeach()
if(rings_group1 GREATER 60 OR rings_off LESS 300)
    message(FATAL_ERROR "after the closed hat the open one rings ${rings_group1} ms choked "
                        "(want under 60) and ${rings_off} ms not (want over 300)")
endif()
message(STATUS "the open hat: ${rings_group1} ms choked, ${rings_off} ms not")

# Gated, the crash ends its 100 ms release after its key; one-shot, it plays on.
hit(49 0.2 "{\"params\":{\"pad14.play\":\"gated\",\"pad14.release\":100}}" peak gated)
hit(49 0.2 "{}" peak oneshot)
if(gated LESS 90 OR gated GREATER 110 OR oneshot LESS 1000)
    message(FATAL_ERROR "the crash sounds ${gated} ms after its key gated (want 100) and "
                        "${oneshot} ms one-shot (want over 1000)")
endif()
message(STATUS "the crash: ${gated} ms after its key gated, ${oneshot} ms one-shot")

# On Out 1, the kick leaves the main output.
hit(36 0.1 "{\"params\":{\"pad1.output\":\"out1\"}}" peak release)
if(peak GREATER -150)
    message(FATAL_ERROR "the kick on Out 1 still peaks at ${peak} dBFS on the main output")
endif()
message(STATUS "the kick on Out 1: silent on the main output")
