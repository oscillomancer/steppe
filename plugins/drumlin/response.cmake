# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Drumlin, measured through the built plugin (dsp.test): its default kit
# sounds on General MIDI's drum keys and nowhere else, a pad's Key moves it,
# Tune plays its recording faster (shorter as well as higher), the closed hat
# chokes the open one, and a pad on an extra output leaves the main one.
# Required: SANDBOX (steppe-sandbox), PLUGIN (Drumlin.clap), WORK.
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
# is expected (a key with no pad).
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

# One hit of `key` with the patch `patch`: its peak (dB, whole) and how long
# it sounds after its key (ms, whole).
function(hit key patch peak release)
    measure("\"seconds\":4,\"notes\":[{\"key\":${key},\"velocity\":0.8,\"start_s\":0,\"length_s\":0.01}],\"patch\":${patch}" json silent)
    string(JSON p GET "${json}" signals 0 peak_dbfs)
    string(JSON r GET "${json}" signals 0 release_ms)
    string(REGEX REPLACE "\\..*" "" p "${p}")
    string(REGEX REPLACE "\\..*" "" r "${r}")
    set(${peak} ${p} PARENT_SCOPE)
    set(${release} ${r} PARENT_SCOPE)
endfunction()

# The default kit: a sound on each pad's key, none on keys no pad has.
foreach(key 36 37 38 39 41 42 45 46 47 49 50 51 54 56 63 69 40 60)
    hit(${key} "{}" peak release)
    if((key EQUAL 40 OR key EQUAL 60) AND peak GREATER -150)
        message(FATAL_ERROR "key ${key}, which no pad has, peaks at ${peak} dBFS")
    elseif(NOT key EQUAL 40 AND NOT key EQUAL 60 AND peak LESS -40)
        message(FATAL_ERROR "the default kit's pad on key ${key} peaks at ${peak} dBFS")
    endif()
endforeach()
message(STATUS "the default kit: General MIDI's drum keys play, 40 and 60 are silent")

# A pad's Key moves it: the kick on key 60 leaves key 36.
hit(60 "{\"params\":{\"pad1.key\":60}}" moved release)
hit(36 "{\"params\":{\"pad1.key\":60}}" left release)
if(moved LESS -40 OR left GREATER -150)
    message(FATAL_ERROR "the kick moved to key 60 peaks at ${moved} dBFS there and ${left} on 36")
endif()
message(STATUS "pad 1 on key 60: ${moved} dBFS there, silent on 36")

# Tune is the machine's clock: an octave up plays the kick's recording in
# half the time.
hit(36 "{}" peak natural)
hit(36 "{\"params\":{\"pad1.tune\":12}}" peak octave)
math(EXPR low "${natural} * 40 / 100")
math(EXPR high "${natural} * 60 / 100")
if(octave LESS low OR octave GREATER high)
    message(FATAL_ERROR "the kick sounds ${natural} ms, and ${octave} ms an octave up (want half)")
endif()
message(STATUS "the kick: ${natural} ms, ${octave} ms an octave up")

# Choke: the open hat, then the closed hat 100 ms on. In their group the open
# hat stops; out of it, it rings on long after both keys.
set(hats "\"notes\":[{\"key\":46,\"velocity\":0.8,\"start_s\":0,\"length_s\":0.05},{\"key\":42,\"velocity\":0.8,\"start_s\":0.1,\"length_s\":0.05}]")
foreach(group group1 off)
    measure("\"seconds\":3,${hats},\"patch\":{\"params\":{\"pad8.choke\":\"${group}\"}}" json)
    string(JSON ms GET "${json}" signals 0 release_ms)
    string(REGEX REPLACE "\\..*" "" ms "${ms}")
    set(rings_${group} ${ms})
endforeach()
if(rings_group1 GREATER 200 OR rings_off LESS 400)
    message(FATAL_ERROR "after the closed hat the open one rings ${rings_group1} ms choked "
                        "(want under 200: the closed hat's own decay) and ${rings_off} ms not "
                        "(want over 400)")
endif()
message(STATUS "the open hat: ${rings_group1} ms choked, ${rings_off} ms not")

# On Out 1, the kick leaves the main output.
hit(36 "{\"params\":{\"pad1.output\":\"out1\"}}" peak release)
if(peak GREATER -150)
    message(FATAL_ERROR "the kick on Out 1 still peaks at ${peak} dBFS on the main output")
endif()
message(STATUS "the kick on Out 1: silent on the main output")
