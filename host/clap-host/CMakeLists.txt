# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
supple_add_library(clap-host
  SOURCES src/clap_host.cpp src/vst3_host.cpp src/platform.cpp src/scan.cpp
  DEPS supple::core-base supple::core-json supple::clap supple::semantic-params-abi supple::vst3-host
  PRIVATE_DEPS ${CMAKE_DL_LIBS} supple::product-info)
target_sources(clap-host PRIVATE src/bridge_client.cpp src/bridge_protocol.cpp src/bridge_platform.cpp src/bridge_child.cpp)
if(WIN32)
  target_sources(clap-host PRIVATE src/child_process_windows.cpp src/editor_platform_windows.cpp)
  target_link_libraries(clap-host PRIVATE User32)
else()
  target_sources(clap-host PRIVATE src/child_process_posix.cpp)
  if(NOT APPLE)
    target_sources(clap-host PRIVATE src/editor_platform_posix.cpp)
  endif()
endif()
if(APPLE)
  enable_language(OBJCXX)
  target_sources(clap-host PRIVATE src/editor_platform_mac.mm)
  set_source_files_properties(src/editor_platform_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-std=c++20")
  target_link_libraries(clap-host PRIVATE "-framework CoreFoundation" "-framework AppKit")
endif()
add_executable(${PRODUCT_SLUG}-scanner tools/scanner_main.cpp)
target_include_directories(${PRODUCT_SLUG}-scanner PRIVATE src)
target_link_libraries(${PRODUCT_SLUG}-scanner PRIVATE supple::clap-host)
supple_target_defaults(${PRODUCT_SLUG}-scanner)

# The script player's mark, which the scan cache leaves out (header only).
target_include_directories(clap-host PRIVATE ${PROJECT_SOURCE_DIR}/plugins/suite/compiler/include)

# The plugin bridge: hosts dedicated or shared isolated plugin instances (ADR-0013).
add_executable(${PRODUCT_SLUG}-bridge bridge/bridge_main.cpp)
if(APPLE)
  set(_bridge_objcxx_options -std=c++20)
  if(SUPPLE_APPLE_RT_NOEXCEPT_WARNING)
    list(APPEND _bridge_objcxx_options -Wno-perf-constraint-implies-noexcept)
  endif()
  set_source_files_properties(bridge/bridge_main.cpp PROPERTIES LANGUAGE OBJCXX
                             COMPILE_OPTIONS "${_bridge_objcxx_options}")
endif()
target_link_libraries(${PRODUCT_SLUG}-bridge PRIVATE supple::clap-host supple::core-dsp)
supple_target_defaults(${PRODUCT_SLUG}-bridge)

if(SUPPLE_BUILD_TESTS)
  add_executable(host-child-fixture tests/child_fixture.cpp)
  target_link_libraries(host-child-fixture PRIVATE supple::clap-host)
  supple_target_defaults(host-child-fixture)
  # Hermetic CLAP fixtures for the host tests.
  add_library(clap-host-fixtures MODULE tests/fixture_plugins.cpp)
  target_link_libraries(clap-host-fixtures PRIVATE supple::core-params supple::core-patch
                        supple::semantic-params-abi)
  if(WIN32)
    target_link_libraries(clap-host-fixtures PRIVATE User32)
  endif()
  set_target_properties(clap-host-fixtures PROPERTIES PREFIX "" SUFFIX ".clap" OUTPUT_NAME "test-fixtures")
  supple_target_defaults(clap-host-fixtures)
  target_compile_options(clap-host-fixtures PRIVATE -Wno-old-style-cast)

  if(SUPPLE_PLATFORM_LINUX)
    set(_host_tests tests/test_clap_host.cpp tests/test_scan.cpp tests/test_main_thread_wake.cpp tests/test_native_host.cpp)
  else()
    set(_host_tests tests/test_native_host.cpp tests/test_main_thread_wake.cpp)
  endif()
  supple_add_tests(clap-host SOURCES ${_host_tests} tests/test_child_process.cpp tests/test_local_host.cpp tests/test_note_output.cpp tests/test_bridge_native.cpp tests/test_editors.cpp tests/test_editor_display.cpp LABELS rt)
  if(APPLE)
    set_source_files_properties(tests/test_editor_display.cpp PROPERTIES LANGUAGE OBJCXX
                               COMPILE_OPTIONS "${_bridge_objcxx_options};-fobjc-arc")
    target_link_libraries(clap-host.tests PRIVATE "-framework CoreGraphics")
  endif()
  add_dependencies(clap-host.tests ${PRODUCT_SLUG}-scanner host-child-fixture)
  target_compile_definitions(clap-host.tests PRIVATE CHILD_FIXTURE_PATH="$<TARGET_FILE:host-child-fixture>")
  target_compile_definitions(clap-host.tests PRIVATE SCANNER_PATH="$<TARGET_FILE:${PRODUCT_SLUG}-scanner>")
  target_compile_definitions(clap-host.tests PRIVATE SUPPLE_TEST_STOCK_EDITORS=$<BOOL:${SUPPLE_BUILD_GUI}>)
  add_dependencies(clap-host.tests clap-host-fixtures ${PRODUCT_SLUG}-bridge)
  target_include_directories(clap-host.tests PRIVATE ${PROJECT_SOURCE_DIR}/plugins/suite/compiler/include)
  target_compile_definitions(clap-host.tests PRIVATE FIXTURE_CLAP_PATH="$<TARGET_FILE:clap-host-fixtures>"
                             BRIDGE_PATH="$<TARGET_FILE:${PRODUCT_SLUG}-bridge>")


  foreach(_mode good crash hang)
    add_library(clap-scan-${_mode} MODULE tests/fixture_scan.cpp)
    target_link_libraries(clap-scan-${_mode} PRIVATE supple::clap)
    set_target_properties(clap-scan-${_mode} PROPERTIES PREFIX "" SUFFIX ".clap")
    supple_target_defaults(clap-scan-${_mode})
    add_dependencies(clap-host.tests clap-scan-${_mode})
  endforeach()
  target_compile_definitions(clap-scan-good PRIVATE SCAN_MODE=0)
  target_compile_definitions(clap-scan-crash PRIVATE SCAN_MODE=1)
  target_compile_definitions(clap-scan-hang PRIVATE SCAN_MODE=2)
  target_compile_definitions(clap-host.tests PRIVATE
    SCAN_GOOD_PATH="$<TARGET_FILE:clap-scan-good>"
    SCAN_CRASH_PATH="$<TARGET_FILE:clap-scan-crash>"
    SCAN_HANG_PATH="$<TARGET_FILE:clap-scan-hang>")

  set(_vst3_fixture_sdk ${CMAKE_SOURCE_DIR}/third_party/vst3sdk-v3.8.0_build_66/public.sdk/source/vst)
  file(GLOB _vst3_fixture_base
    ${CMAKE_SOURCE_DIR}/third_party/vst3sdk-v3.8.0_build_66/base/source/*.cpp)
  file(GLOB _vst3_fixture_threads
    ${CMAKE_SOURCE_DIR}/third_party/vst3sdk-v3.8.0_build_66/base/thread/source/*.cpp)
  add_library(clap-host-vst3-fixtures MODULE
    tests/fixture_vst3.cpp
    ${_vst3_fixture_base}
    ${_vst3_fixture_threads}
    ${CMAKE_SOURCE_DIR}/third_party/vst3sdk-v3.8.0_build_66/public.sdk/source/common/commoniids.cpp
    ${CMAKE_SOURCE_DIR}/third_party/vst3sdk-v3.8.0_build_66/public.sdk/source/common/pluginview.cpp
    ${_vst3_fixture_sdk}/vstsinglecomponenteffect.cpp
    ${_vst3_fixture_sdk}/vstbus.cpp
    ${_vst3_fixture_sdk}/vstcomponentbase.cpp
    ${_vst3_fixture_sdk}/vstparameters.cpp)
  target_link_libraries(clap-host-vst3-fixtures PRIVATE supple::vst3-host)
  if(APPLE)
    set(_vst3_arch MacOS)
    set(_vst3_suffix "")
  elseif(WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
      set(_vst3_arch arm64-win)
    else()
      set(_vst3_arch x86_64-win)
    endif()
    set(_vst3_suffix ".vst3")
  else()
    set(_vst3_arch x86_64-linux)
    set(_vst3_suffix ".so")
  endif()
  set_target_properties(clap-host-vst3-fixtures PROPERTIES PREFIX "" SUFFIX "${_vst3_suffix}"
    OUTPUT_NAME "test-vst3"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-vst3.vst3/Contents/${_vst3_arch}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-vst3.vst3/Contents/${_vst3_arch}"
    WINDOWS_EXPORT_ALL_SYMBOLS ON)
  if(APPLE)
    configure_file(tests/fixture_Info.plist "${CMAKE_CURRENT_BINARY_DIR}/test-vst3.vst3/Contents/Info.plist" COPYONLY)
  endif()
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-vst3.vst3/Contents/Resources")
  configure_file(tests/fixture_moduleinfo.json
    "${CMAKE_CURRENT_BINARY_DIR}/test-vst3.vst3/Contents/Resources/moduleinfo.json" COPYONLY)
  target_compile_options(clap-host-vst3-fixtures PRIVATE -w)
  target_compile_definitions(clap-host-vst3-fixtures PRIVATE DEVELOPMENT=1)
  add_dependencies(clap-host.tests clap-host-vst3-fixtures)
  target_compile_definitions(clap-host.tests PRIVATE
    FIXTURE_VST3_PATH="${CMAKE_CURRENT_BINARY_DIR}/test-vst3.vst3"
    FIXTURE_VST3_MODULE="$<TARGET_FILE:clap-host-vst3-fixtures>")
endif()

if(APPLE AND SUPPLE_BUILD_TESTS)
  # Exercise actual executable-relative lookup from the GUI bundle's depth,
  # rather than relying on a build-tree symlink to keep its old location.
  set(bundle_probe_dir "${CMAKE_BINARY_DIR}/daw/gui/host-lookup-test.app/Contents/MacOS")
  file(MAKE_DIRECTORY "${bundle_probe_dir}")
  add_custom_command(TARGET clap-host.tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:clap-host.tests>" "${bundle_probe_dir}/host-lookup-tests")
  add_test(NAME clap-host.bundle-lookup COMMAND "${bundle_probe_dir}/host-lookup-tests"
    "--test-case=bridge lookup resolves*,the agent skill ships*")
  set_tests_properties(clap-host.bundle-lookup PROPERTIES LABELS clap-host)
endif()
