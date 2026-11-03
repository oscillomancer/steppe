# Vendored third-party code. Every directory here must have an entry in
# third_party/manifest.json and THIRD_PARTY.md (tools/license-check enforces
# this). Warnings are never enabled for these targets.

add_library(tp-clap INTERFACE)
target_include_directories(tp-clap SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/clap/include)
add_library(supple::clap ALIAS tp-clap)

add_library(tp-doctest INTERFACE)
target_include_directories(tp-doctest SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/doctest/include)
add_library(supple::doctest ALIAS tp-doctest)

add_library(tp-fast-float INTERFACE)
target_include_directories(tp-fast-float SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/fast_float/include)
add_library(supple::fast-float ALIAS tp-fast-float)

add_library(tp-doctest-main STATIC ${CMAKE_CURRENT_SOURCE_DIR}/doctest_main.cpp)
target_link_libraries(tp-doctest-main PUBLIC tp-doctest)
target_compile_features(tp-doctest-main PUBLIC cxx_std_20)
if(_supple_sanitize_flags)
  target_compile_options(tp-doctest-main PRIVATE ${_supple_sanitize_flags})
endif()
add_library(supple::doctest-main ALIAS tp-doctest-main)

add_library(tp-dr-libs INTERFACE)
target_include_directories(tp-dr-libs SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/dr_libs/include)
add_library(supple::dr-libs ALIAS tp-dr-libs)

add_library(tp-pocketfft INTERFACE)
target_include_directories(tp-pocketfft SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/pocketfft/include)
target_compile_definitions(tp-pocketfft INTERFACE POCKETFFT_NO_MULTITHREADING)
add_library(supple::pocketfft ALIAS tp-pocketfft)

# Signalsmith Stretch and its STFT/FFT layer Signalsmith Linear (both MIT):
# offline, pitch-preserving time-stretch of media takes (engine.md §4). Header
# only, FFT backends off (the platform headers are only reached through
# SIGNALSMITH_USE_* definitions, which nobody sets).
add_library(tp-signalsmith-stretch INTERFACE)
target_include_directories(tp-signalsmith-stretch SYSTEM INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/signalsmith-stretch/include
  ${CMAKE_CURRENT_SOURCE_DIR}/signalsmith-linear/include)
target_compile_features(tp-signalsmith-stretch INTERFACE cxx_std_20)
add_library(supple::signalsmith-stretch ALIAS tp-signalsmith-stretch)

# PUC Lua 5.4 (MIT) — the embedded scripting language (ADR-0006). Only the
# core and the libraries our sandbox opens are compiled: io, os, debug and
# package are not built at all, so a script cannot reach them (libs/lua-bus).
# Warnings are never enabled for third-party targets (see the file header).
set(_lua ${CMAKE_CURRENT_SOURCE_DIR}/lua-5.4.9/src)
add_library(tp-lua STATIC
  ${_lua}/lapi.c ${_lua}/lcode.c ${_lua}/lctype.c ${_lua}/ldebug.c ${_lua}/ldo.c
  ${_lua}/ldump.c ${_lua}/lfunc.c ${_lua}/lgc.c ${_lua}/llex.c ${_lua}/lmem.c
  ${_lua}/lobject.c ${_lua}/lopcodes.c ${_lua}/lparser.c ${_lua}/lstate.c
  ${_lua}/lstring.c ${_lua}/ltable.c ${_lua}/ltm.c ${_lua}/lundump.c ${_lua}/lvm.c
  ${_lua}/lzio.c
  ${_lua}/lauxlib.c ${_lua}/lbaselib.c ${_lua}/lcorolib.c ${_lua}/lmathlib.c
  ${_lua}/lstrlib.c ${_lua}/ltablib.c ${_lua}/lutf8lib.c)
# Compiled as C++, so a Lua error is a C++ exception rather than a longjmp:
# it unwinds the C++ frames between the raise and the pcall (bindings,
# conversions) with their destructors, and ThreadSanitizer can follow it.
# Consumers include lua.h/lauxlib.h/lualib.h directly, never lua.hpp (its
# extern "C" would not match the C++ symbols).
get_target_property(_lua_sources tp-lua SOURCES)
set_source_files_properties(${_lua_sources} PROPERTIES LANGUAGE CXX)
target_include_directories(tp-lua SYSTEM PUBLIC ${_lua})
target_compile_features(tp-lua PUBLIC cxx_std_20)
set_target_properties(tp-lua PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(_supple_sanitize_flags)
  target_compile_options(tp-lua PRIVATE ${_supple_sanitize_flags})
endif()
target_link_libraries(tp-lua PUBLIC ${CMAKE_DL_LIBS})
add_library(supple::lua ALIAS tp-lua)

if(SUPPLE_PLUGIN_HOST)
set(_vst3 ${CMAKE_CURRENT_SOURCE_DIR}/vst3sdk-v3.8.0_build_66)
add_library(tp-vst3-host STATIC
  ${_vst3}/pluginterfaces/base/coreiids.cpp
  ${_vst3}/pluginterfaces/base/funknown.cpp
  ${_vst3}/pluginterfaces/base/ustring.cpp
  ${_vst3}/public.sdk/source/common/commoniids.cpp
  ${_vst3}/public.sdk/source/common/commonstringconvert.cpp
  ${_vst3}/public.sdk/source/vst/vstinitiids.cpp
  ${_vst3}/public.sdk/source/vst/hosting/module.cpp
  ${_vst3}/public.sdk/source/vst/hosting/eventlist.cpp
  ${_vst3}/public.sdk/source/vst/hosting/hostclasses.cpp
  ${_vst3}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
  ${_vst3}/public.sdk/source/vst/hosting/parameterchanges.cpp
  ${_vst3}/public.sdk/source/vst/utility/stringconvert.cpp)
if(APPLE)
  enable_language(OBJCXX)
  set_target_properties(tp-vst3-host PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
  target_sources(tp-vst3-host PRIVATE ${_vst3}/public.sdk/source/vst/hosting/module_mac.mm)
  target_compile_options(tp-vst3-host PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
  target_link_libraries(tp-vst3-host PUBLIC "-framework Cocoa" "-framework CoreFoundation")
elseif(WIN32)
  target_sources(tp-vst3-host PRIVATE ${_vst3}/public.sdk/source/vst/hosting/module_win32.cpp)
  target_link_libraries(tp-vst3-host PUBLIC shell32 ole32 uuid)
else()
  target_sources(tp-vst3-host PRIVATE ${_vst3}/public.sdk/source/vst/hosting/module_linux.cpp)
endif()
target_include_directories(tp-vst3-host SYSTEM PUBLIC ${_vst3})
target_compile_features(tp-vst3-host PUBLIC cxx_std_20)
set_target_properties(tp-vst3-host PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(_supple_sanitize_flags)
  target_compile_options(tp-vst3-host PRIVATE ${_supple_sanitize_flags})
endif()
target_link_libraries(tp-vst3-host PUBLIC ${CMAKE_DL_LIBS})
add_library(supple::vst3-host ALIAS tp-vst3-host)
endif()

# MIR 1.0.0 (MIT): the JIT and interpreter behind DSP scripts (ADR-0032).
# Only the core, the interpreter (included by mir.c) and the generator are
# built; each pulls in its target's back end by itself. GNU C11, as upstream
# builds it.
set(_mir ${CMAKE_CURRENT_SOURCE_DIR}/mir-1.0.0)
add_library(tp-mir STATIC ${_mir}/mir.c ${_mir}/mir-gen.c)
target_include_directories(tp-mir SYSTEM PUBLIC ${_mir})
set_target_properties(tp-mir PROPERTIES C_EXTENSIONS ON POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(tp-mir PRIVATE /clang:-w /clang:-fsigned-char /clang:-std=gnu11)
else()
  set_target_properties(tp-mir PROPERTIES C_STANDARD 11)
  target_compile_options(tp-mir PRIVATE -w -fsigned-char)
endif()
if(_supple_sanitize_flags)
  target_compile_options(tp-mir PRIVATE ${_supple_sanitize_flags})
  # MIR's string hash reads unaligned 64-bit words (mir-hash.h), which
  # x86-64 and AArch64 allow but UBSan's alignment check reports.
  if(SUPPLE_SANITIZE MATCHES "undefined")
    target_compile_options(tp-mir PRIVATE -fno-sanitize=alignment)
  endif()
endif()
add_library(supple::mir ALIAS tp-mir)

# Render encoders for the DAW (ADR-0055): libogg 1.3.6 and libvorbis 1.3.7
# (BSD-3-Clause) for Ogg Vorbis, LAME 3.100 (LGPL-2.0-or-later, the approved
# exception of ADR-0055 §8) for MP3. Linked only by libs/audio-codecs, which
# only the engine links: never plugins/ or the MIT libraries. Config headers
# are ours (lame-3.100/config, libogg's config_types.h); no autotools, no
# assembly or SSE, so every platform builds the same C code. Floating-point
# contraction is off like the rest of the build, which keeps encodes stable.
function(_supple_codec_target target)
  set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  if(MSVC)
    target_compile_options(${target} PRIVATE /w /clang:-ffp-contract=off)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
  else()
    set_target_properties(${target} PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
    target_compile_options(${target} PRIVATE -w -ffp-contract=off)
  endif()
  if(_supple_sanitize_flags)
    target_compile_options(${target} PRIVATE ${_supple_sanitize_flags})
  endif()
endfunction()

set(_ogg ${CMAKE_CURRENT_SOURCE_DIR}/libogg-1.3.6)
add_library(tp-ogg STATIC ${_ogg}/src/bitwise.c ${_ogg}/src/framing.c)
target_include_directories(tp-ogg SYSTEM PUBLIC ${_ogg}/include)
_supple_codec_target(tp-ogg)
add_library(supple::ogg ALIAS tp-ogg)

set(_vorbis ${CMAKE_CURRENT_SOURCE_DIR}/libvorbis-1.3.7)
add_library(tp-vorbis STATIC
  ${_vorbis}/lib/analysis.c ${_vorbis}/lib/bitrate.c ${_vorbis}/lib/block.c
  ${_vorbis}/lib/codebook.c ${_vorbis}/lib/envelope.c ${_vorbis}/lib/floor0.c
  ${_vorbis}/lib/floor1.c ${_vorbis}/lib/info.c ${_vorbis}/lib/lookup.c ${_vorbis}/lib/lpc.c
  ${_vorbis}/lib/lsp.c ${_vorbis}/lib/mapping0.c ${_vorbis}/lib/mdct.c ${_vorbis}/lib/psy.c
  ${_vorbis}/lib/registry.c ${_vorbis}/lib/res0.c ${_vorbis}/lib/sharedbook.c
  ${_vorbis}/lib/smallft.c ${_vorbis}/lib/synthesis.c ${_vorbis}/lib/window.c
  ${_vorbis}/lib/vorbisenc.c ${_vorbis}/lib/vorbisfile.c)
target_include_directories(tp-vorbis SYSTEM PUBLIC ${_vorbis}/include)
target_include_directories(tp-vorbis PRIVATE ${_vorbis}/lib)
target_link_libraries(tp-vorbis PUBLIC tp-ogg)
if(NOT WIN32)
  target_compile_definitions(tp-vorbis PRIVATE HAVE_ALLOCA_H=1)
  target_link_libraries(tp-vorbis PUBLIC m)
endif()
_supple_codec_target(tp-vorbis)
add_library(supple::vorbis ALIAS tp-vorbis)

set(_lame ${CMAKE_CURRENT_SOURCE_DIR}/lame-3.100)
add_library(tp-lame STATIC
  ${_lame}/libmp3lame/bitstream.c ${_lame}/libmp3lame/encoder.c ${_lame}/libmp3lame/fft.c
  ${_lame}/libmp3lame/gain_analysis.c ${_lame}/libmp3lame/id3tag.c ${_lame}/libmp3lame/lame.c
  ${_lame}/libmp3lame/newmdct.c ${_lame}/libmp3lame/presets.c ${_lame}/libmp3lame/psymodel.c
  ${_lame}/libmp3lame/quantize.c ${_lame}/libmp3lame/quantize_pvt.c
  ${_lame}/libmp3lame/reservoir.c ${_lame}/libmp3lame/set_get.c ${_lame}/libmp3lame/tables.c
  ${_lame}/libmp3lame/takehiro.c ${_lame}/libmp3lame/util.c ${_lame}/libmp3lame/vbrquantize.c
  ${_lame}/libmp3lame/VbrTag.c ${_lame}/libmp3lame/version.c)
target_include_directories(tp-lame SYSTEM PUBLIC ${_lame}/include)
target_include_directories(tp-lame PRIVATE ${_lame}/config ${_lame}/libmp3lame)
target_compile_definitions(tp-lame PRIVATE HAVE_CONFIG_H=1)
if(NOT WIN32)
  target_link_libraries(tp-lame PUBLIC m)
endif()
_supple_codec_target(tp-lame)
add_library(supple::lame ALIAS tp-lame)
