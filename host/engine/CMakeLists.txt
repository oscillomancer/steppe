# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
add_subdirectory(model)

# dsp.preview writes PNGs with the system zlib (host/dsp-commands/png.h).
find_package(ZLIB REQUIRED)

supple_add_library(engine
  SOURCES src/transients.cpp src/audio_commands.cpp src/prepared_sources.cpp src/offline.cpp src/plan.cpp src/workers.cpp src/live_workers.cpp src/realtime.cpp src/recording.cpp
          src/stretch.cpp src/playrate_dsp.cpp src/playrate_timeline.cpp src/rate_sources.cpp src/rate_runtime.cpp src/mix_runtime.cpp src/meters.cpp src/runtime_protection.cpp src/media_files.cpp
          src/rack_runtime.cpp src/rack_midi.cpp src/rack_clip_commands.cpp src/rack_level_commands.cpp src/rack_commands.cpp src/module_library.cpp src/module_commands.cpp src/rack_probe.cpp src/app.cpp src/plugin_outputs.cpp src/commands.cpp src/extra_commands.cpp src/group_commands.cpp src/midi_commands.cpp src/analysis_commands.cpp
          src/freeze_commands.cpp src/consolidate_commands.cpp src/export_commands.cpp src/mix_commands.cpp src/heal_commands.cpp src/render_commands.cpp src/render_settings.cpp src/render_chain.cpp src/chain_commands.cpp src/take_commands.cpp src/learn.cpp src/control_learn.cpp src/dsp_commands.cpp
          src/midi_control.cpp src/midi_input.cpp src/portaudio_backend.cpp src/portaudio_native.cpp
  DEPS supple::model supple::clap-host supple::audio-file supple::timeline supple::semantic-params-abi
       supple::command-bus supple::lua-bus supple::notation supple::core-params supple::core-analysis
       supple::core-dsp supple::signalsmith-stretch supple::console9 supple::rack
  PRIVATE_DEPS supple::rack-stock supple::product-info supple::script-compiler supple::sandbox-render supple::audio-codecs ZLIB::ZLIB ${CMAKE_DL_LIBS})

target_include_directories(engine SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/portaudio-19.7.0/include)
if(APPLE)
  enable_language(OBJCXX)
  target_sources(engine PRIVATE src/midi_input_mac.mm)
  set(_midi_objcxx_options -fobjc-arc -std=c++20)
  if(SUPPLE_APPLE_RT_NOEXCEPT_WARNING)
    list(APPEND _midi_objcxx_options -Wno-perf-constraint-implies-noexcept)
  endif()
  set_source_files_properties(src/midi_input_mac.mm PROPERTIES COMPILE_OPTIONS "${_midi_objcxx_options}")
  target_link_libraries(engine PRIVATE "-framework CoreMIDI" "-framework CoreFoundation")
elseif(WIN32)
  target_sources(engine PRIVATE src/midi_input_windows.cpp)
  target_link_libraries(engine PRIVATE winmm)
endif()
if(APPLE OR WIN32)
  target_link_libraries(engine PRIVATE supple::portaudio)
  if(WIN32)
    target_link_libraries(engine PRIVATE avrt)
    target_include_directories(engine SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/asiosdk-2.3.4/common)
  endif()
endif()

if(SUPPLE_PLATFORM_LINUX)
  target_sources(engine PRIVATE src/jack_backend.cpp)
endif()

if(SUPPLE_BUILD_TESTS AND SUPPLE_PLATFORM_LINUX)
  # A stand-in libjack: the JACK backend is tested without a server or device.
  add_library(engine-fake-jack MODULE tests/fake_jack.cpp)
  set_target_properties(engine-fake-jack PROPERTIES PREFIX "" OUTPUT_NAME "fake-libjack")
  supple_target_defaults(engine-fake-jack)
  # Its handle types are its own (as a real libjack's, which is C and carries
  # no signature for -fsanitize=function to check).
  target_compile_options(engine-fake-jack PRIVATE $<$<CXX_COMPILER_ID:Clang>:-fno-sanitize=function>)

  supple_add_tests(engine SOURCES tests/test_item_edits.cpp tests/test_audio_warp.cpp tests/test_offline.cpp tests/test_freeze.cpp tests/test_instance_pool.cpp tests/test_autosave.cpp tests/test_take_copy.cpp tests/test_groups.cpp tests/test_media_files.cpp tests/test_recording_shutdown.cpp tests/test_realtime.cpp tests/test_app.cpp
                   tests/test_commands.cpp tests/test_scripts.cpp tests/test_strips.cpp tests/test_playrate.cpp tests/test_mix_edits.cpp tests/test_pdc.cpp
                   tests/test_runtime_protection.cpp tests/test_jack.cpp tests/test_workers.cpp tests/test_live_workers.cpp tests/test_meters.cpp
                   tests/test_learn.cpp tests/test_dsp.cpp tests/test_bridge_engine.cpp tests/test_render.cpp tests/test_racks.cpp tests/test_rack_clip.cpp tests/test_rack_levels.cpp tests/test_module_library.cpp tests/test_rack_recipes.cpp tests/test_rack_patches.cpp LABELS rt SPLIT)
  target_include_directories(engine.tests PRIVATE src)
  target_include_directories(engine.tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/fuzz)
  add_dependencies(engine.tests clap-host-fixtures clap-host-vst3-fixtures engine-fake-jack ${PRODUCT_SLUG}-bridge)
  target_compile_definitions(engine.tests PRIVATE FIXTURE_CLAP_PATH="$<TARGET_FILE:clap-host-fixtures>"
                             FIXTURE_VST3_PATH="${CMAKE_BINARY_DIR}/host/clap-host/test-vst3.vst3"
                             FAKE_JACK_PATH="$<TARGET_FILE:engine-fake-jack>"
                             BRIDGE_PATH="$<TARGET_FILE:${PRODUCT_SLUG}-bridge>"
                             STOCK_CLAP_DIR="${CMAKE_BINARY_DIR}/plugins/clap"
                             SUPPLE_SCRIPT_EXAMPLES="${PROJECT_SOURCE_DIR}/plugins/suite/examples"
                             SUPPLE_RACK_GUIDE="${PROJECT_SOURCE_DIR}/skill/guides/rack.md")
  target_link_libraries(engine.tests PRIVATE ${CMAKE_DL_LIBS})
endif()

if(SUPPLE_BUILD_TESTS AND NOT SUPPLE_PLATFORM_LINUX)
  supple_add_tests(engine SOURCES tests/test_item_edits.cpp tests/test_audio_warp.cpp tests/test_native_engine.cpp tests/test_freeze.cpp tests/test_instance_pool.cpp tests/test_autosave.cpp tests/test_take_copy.cpp tests/test_groups.cpp tests/test_media_files.cpp tests/test_recording_shutdown.cpp tests/test_bridge_engine.cpp tests/test_playrate.cpp tests/test_mix_edits.cpp tests/test_pdc.cpp tests/test_render.cpp tests/test_racks.cpp tests/test_rack_clip.cpp tests/test_rack_levels.cpp tests/test_module_library.cpp tests/test_rack_recipes.cpp tests/test_rack_patches.cpp LABELS rt)
  target_include_directories(engine.tests PRIVATE src)
  add_dependencies(engine.tests clap-host-fixtures ${PRODUCT_SLUG}-scanner ${PRODUCT_SLUG}-bridge)
  target_compile_definitions(engine.tests PRIVATE FIXTURE_CLAP_PATH="$<TARGET_FILE:clap-host-fixtures>" BRIDGE_PATH="$<TARGET_FILE:${PRODUCT_SLUG}-bridge>"
                             STOCK_CLAP_DIR="${CMAKE_BINARY_DIR}/plugins/clap"
                             SUPPLE_RACK_GUIDE="${PROJECT_SOURCE_DIR}/skill/guides/rack.md")
endif()

if(SUPPLE_PLATFORM_LINUX)
# Capacity benchmark (engine.md §7 B1), with a command-built shared session.
add_executable(engine-bench bench/engine_bench.cpp bench/session.cpp)
target_link_libraries(engine-bench PRIVATE supple::engine supple::product-info)
target_include_directories(engine-bench PRIVATE src)
supple_target_defaults(engine-bench)
target_compile_definitions(engine-bench PRIVATE
  BENCH_DRIVER="${PROJECT_SOURCE_DIR}/tools/bench/run.py"
  BENCH_RECIPE="${CMAKE_CURRENT_SOURCE_DIR}/bench/session.json"
  BENCH_PLUGINS="${CMAKE_BINARY_DIR}/plugins/clap")
if(SUPPLE_BUILD_TESTS)
  target_sources(engine.tests PRIVATE tests/test_bench.cpp bench/session.cpp)
  target_include_directories(engine.tests PRIVATE bench)
  target_compile_definitions(engine.tests PRIVATE BENCH_RECIPE="${CMAKE_CURRENT_SOURCE_DIR}/bench/session.json"
                             SUPPLE_EDITORS=$<BOOL:${SUPPLE_BUILD_GUI}>)
  add_test(NAME engine.tests.test_bench COMMAND engine.tests "-sf=*/test_bench.cpp")
  set_tests_properties(engine.tests.test_bench PROPERTIES LABELS "engine;rt" ENVIRONMENT
    "RTSAN_OPTIONS=halt_on_error=1;UBSAN_OPTIONS=print_stacktrace=1")
  add_executable(engine-bench-lua-check bench/lua_check.cpp)
  target_link_libraries(engine-bench-lua-check PRIVATE supple::lua)
  supple_target_defaults(engine-bench-lua-check)
  add_dependencies(engine.tests engine-bench-lua-check)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME engine.bench-tooling COMMAND ${Python3_EXECUTABLE} -m unittest discover
    -s ${PROJECT_SOURCE_DIR}/tools/bench -p "test_*.py")
  set_tests_properties(engine.bench-tooling PROPERTIES ENVIRONMENT
    "SUPPLE_BENCH_LUA_CHECK=$<TARGET_FILE:engine-bench-lua-check>")
endif()

endif()

# The headless DAW CLI: <slug>-engine (named from cmake/Product.cmake).
add_executable(${PRODUCT_SLUG}-engine tools/engine_main.cpp)
target_link_libraries(${PRODUCT_SLUG}-engine PRIVATE supple::engine supple::mcp supple::product-info)
supple_target_defaults(${PRODUCT_SLUG}-engine)

if(SUPPLE_BUILD_TESTS)
  add_test(NAME engine.registry-drift
    COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:${PRODUCT_SLUG}-engine>
            -DGENERATED=${PROJECT_SOURCE_DIR}/skill/generated -DPREFIX=engine
            -P ${PROJECT_SOURCE_DIR}/host/sandbox/tools/check_generated.cmake)
endif()

if(SUPPLE_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME engine.cli-numbers
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py
            $<TARGET_FILE:${PRODUCT_SLUG}-engine>)
  add_test(NAME engine.mcp-stdio
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/mcp_client.py
            $<TARGET_FILE:${PRODUCT_SLUG}-engine> session_describe)
  add_test(NAME engine.mcp-script
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/mcp_script.py
            $<TARGET_FILE:${PRODUCT_SLUG}-engine> engine)
  add_test(NAME engine.mcp-proxy
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/mcp_proxy.py
            $<TARGET_FILE:${PRODUCT_SLUG}-engine> $<TARGET_FILE:${PRODUCT_SLUG}-mcp> --engine)
  # Phase 4 acceptance: an arrangement built and rendered over MCP with the
  # stock synth (dev and GCC debug presets only: a full synth render).
  if(SUPPLE_BUILD_PLUGINS AND CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT SUPPLE_SANITIZE)
    add_test(NAME engine.mcp-arrangement
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/mcp_arrangement.py
              $<TARGET_FILE:${PRODUCT_SLUG}-engine> ${CMAKE_BINARY_DIR}/plugins/clap/Marram.clap)
    set_tests_properties(engine.mcp-arrangement PROPERTIES LABELS "engine;slow")
  endif()
endif()

if(SUPPLE_BUILD_TESTS)
  add_executable(engine-portaudio.tests tests/test_portaudio.cpp)
  target_link_libraries(engine-portaudio.tests PRIVATE supple::engine supple::doctest-main)
  target_include_directories(engine-portaudio.tests PRIVATE src)
  target_include_directories(engine-portaudio.tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/portaudio-19.7.0/include)
  target_compile_definitions(engine-portaudio.tests PRIVATE FIXTURE_CLAP_PATH="$<TARGET_FILE:clap-host-fixtures>")
  add_dependencies(engine-portaudio.tests clap-host-fixtures)
  supple_target_defaults(engine-portaudio.tests)
  add_test(NAME engine-portaudio.tests COMMAND engine-portaudio.tests)
  set_tests_properties(engine-portaudio.tests PROPERTIES LABELS "engine;rt")
endif()

if(SUPPLE_BUILD_TESTS)
  add_executable(engine-dsp-link.tests tests/test_dsp_link.cpp)
  target_link_libraries(engine-dsp-link.tests PRIVATE supple::core-dsp supple::script-runtime supple::doctest-main)
  supple_target_defaults(engine-dsp-link.tests)
  add_test(NAME engine-dsp-link.tests COMMAND engine-dsp-link.tests)
  set_tests_properties(engine-dsp-link.tests PROPERTIES LABELS "engine;rt")
endif()

if(SUPPLE_BUILD_TESTS)
  # Portable DSP tests, including under RTSan, with no devices/plugins.
  add_executable(engine-playrate-dsp.tests tests/test_playrate_dsp.cpp)
  target_link_libraries(engine-playrate-dsp.tests PRIVATE supple::engine supple::doctest-main)
  target_include_directories(engine-playrate-dsp.tests PRIVATE src)
  supple_target_defaults(engine-playrate-dsp.tests)
  add_test(NAME engine-playrate-dsp.tests COMMAND engine-playrate-dsp.tests)
  set_tests_properties(engine-playrate-dsp.tests PROPERTIES LABELS "engine;rt")
endif()

# Pure in-memory source-conversion CPU probe, never opens an audio device.
add_executable(engine-playrate-bench bench/playrate_bench.cpp)
target_include_directories(engine-playrate-bench PRIVATE src)
target_link_libraries(engine-playrate-bench PRIVATE supple::engine)
supple_target_defaults(engine-playrate-bench)

add_executable(engine-playrate-project-bench bench/playrate_project_bench.cpp)
target_include_directories(engine-playrate-project-bench PRIVATE src)
target_link_libraries(engine-playrate-project-bench PRIVATE supple::engine)
supple_target_defaults(engine-playrate-project-bench)
