# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
# The offline renderer on its own, so that the engine's dsp.test renders as the
# sandbox does (host/dsp-commands/dsp_test.h).
supple_add_library(sandbox-render
  SOURCES src/render.cpp
  DEPS supple::clap-host supple::audio-file supple::core-dsp)

# dsp.preview writes PNGs with the system zlib (host/dsp-commands/png.h).
find_package(ZLIB REQUIRED)

supple_add_library(sandbox
  SOURCES src/session.cpp src/commands.cpp src/analysis_commands.cpp src/optimize.cpp
          src/dsp_commands.cpp
  DEPS supple::sandbox-render supple::clap-host supple::audio-file supple::core-json supple::midi
       supple::notation supple::command-bus supple::lua-bus supple::semantic-params-abi
       supple::core-analysis supple::core-dsp supple::optimize supple::core-params
  PRIVATE_DEPS supple::script-compiler ZLIB::ZLIB)

if(SUPPLE_BUILD_TESTS)
  supple_add_tests(sandbox SOURCES tests/test_render.cpp tests/test_session.cpp tests/test_commands.cpp
                   tests/test_scripts.cpp tests/test_dsp.cpp LABELS rt)
  target_link_libraries(sandbox.tests PRIVATE supple::script-compiler)
  add_dependencies(sandbox.tests clap-host-fixtures clap-host-vst3-fixtures ${PRODUCT_SLUG}-bridge)
  target_compile_definitions(sandbox.tests PRIVATE BRIDGE_PATH="$<TARGET_FILE:${PRODUCT_SLUG}-bridge>"
                             FIXTURE_CLAP_PATH="$<TARGET_FILE:clap-host-fixtures>"
                             FIXTURE_VST3_PATH="${CMAKE_BINARY_DIR}/host/clap-host/test-vst3.vst3"
                             STOCK_CLAP_DIR="${CMAKE_BINARY_DIR}/plugins/clap"
                             SUPPLE_SCRIPT_EXAMPLES="${PROJECT_SOURCE_DIR}/plugins/suite/examples"
                             SUPPLE_SKILL_GUIDES="${PROJECT_SOURCE_DIR}/skill/guides"
                             SUPPLE_EDITORS=$<BOOL:${SUPPLE_BUILD_GUI}>)
  # The optimizer end to end with the real synth (slow-ish; single-threaded).
  add_executable(sandbox-optimize.tests tests/test_optimize.cpp)
  target_link_libraries(sandbox-optimize.tests PRIVATE supple::sandbox supple::doctest-main)
  supple_target_defaults(sandbox-optimize.tests)
  add_test(NAME sandbox-optimize.tests COMMAND sandbox-optimize.tests)
  set_tests_properties(sandbox-optimize.tests PROPERTIES LABELS "sandbox;slow;tsan-skip")
  # The optimizer renders the real synth hundreds of times and scores every render. Under ASan and
  # UBSan the suite takes about ten times the dev run (376 s alone against 36 s) and went over the
  # preset's 600 s on a loaded machine, so that preset gets a safety stop with room (as the stepped
  # reset checks do under TSan); the others keep the preset's.
  if(SUPPLE_SANITIZE MATCHES "address")
    set_property(TEST sandbox-optimize.tests PROPERTY TIMEOUT 1800)
  endif()
  # Marram comes from the stock plugins' build (plugins/clap).
  add_dependencies(sandbox-optimize.tests clap-host-fixtures)
  target_compile_definitions(sandbox-optimize.tests PRIVATE
    STOCK_CLAP_DIR="${CMAKE_BINARY_DIR}/plugins/clap"
    FIXTURE_CLAP_PATH="$<TARGET_FILE:clap-host-fixtures>")
endif()

# The sandbox CLI: <slug>-sandbox (named from cmake/Product.cmake).
add_executable(${PRODUCT_SLUG}-sandbox tools/sandbox_main.cpp)
target_link_libraries(${PRODUCT_SLUG}-sandbox PRIVATE supple::sandbox supple::mcp supple::product-info)
supple_target_defaults(${PRODUCT_SLUG}-sandbox)

if(SUPPLE_BUILD_TESTS)
  # Drift test: the committed generator output must match the registry.
  add_test(NAME sandbox.registry-drift
    COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:${PRODUCT_SLUG}-sandbox>
            -DGENERATED=${PROJECT_SOURCE_DIR}/skill/generated
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_generated.cmake)
endif()

if(SUPPLE_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME sandbox.dsp-discovery
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dsp_discovery.py
            $<TARGET_FILE:${PRODUCT_SLUG}-sandbox>)
  add_test(NAME sandbox.mcp-stdio
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/mcp_client.py
            $<TARGET_FILE:${PRODUCT_SLUG}-sandbox> session_get)
  add_test(NAME sandbox.mcp-script
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/mcp_script.py
            $<TARGET_FILE:${PRODUCT_SLUG}-sandbox> sandbox)
  add_test(NAME sandbox.mcp-proxy
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/mcp_proxy.py
            $<TARGET_FILE:${PRODUCT_SLUG}-sandbox> $<TARGET_FILE:${PRODUCT_SLUG}-mcp>
            ${CMAKE_BINARY_DIR}/plugins/clap/Marram.clap)
  # Keep the Linux documentation golden; native DSP numbers may differ.
  # Both deployments still run every structured-result assertion.
  if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT SUPPLE_SANITIZE)
    set(_mcp_demo_args)
    if(NOT SUPPLE_PLATFORM_LINUX)
      list(APPEND _mcp_demo_args --portable)
    endif()
    add_test(NAME mcp.demo
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/mcp_demo.py
              $<TARGET_FILE:${PRODUCT_SLUG}-sandbox> $<TARGET_FILE:${PRODUCT_SLUG}-mcp>
              ${CMAKE_BINARY_DIR}/plugins/clap/Marram.clap
              ${PROJECT_SOURCE_DIR}/docs/demos/mcp-synth-session.md ${_mcp_demo_args})
    set_tests_properties(mcp.demo PROPERTIES LABELS "sandbox;slow")
  endif()
endif()
