# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
# Cross-library tests: rt-safety gate, integration, fuzz, golden renders.
if(SUPPLE_BUILD_TESTS AND SUPPLE_SANITIZE MATCHES "realtime")
  add_executable(rtsan-selfcheck rt-safety/rtsan_selfcheck.cpp)
  target_link_libraries(rtsan-selfcheck PRIVATE supple::core-base)
  supple_target_defaults(rtsan-selfcheck)
  target_compile_options(rtsan-selfcheck PRIVATE -Wno-function-effects)
  add_test(NAME rtsan-selfcheck COMMAND rtsan-selfcheck)
  set_tests_properties(rtsan-selfcheck PROPERTIES WILL_FAIL TRUE)

  # An inventory announcement on an audio path is caught even without a sink (topic 5c).
  add_executable(rtsan-announce-check rt-safety/rtsan_announce_check.cpp)
  target_link_libraries(rtsan-announce-check PRIVATE supple::core-base supple::diagnostics-context)
  supple_target_defaults(rtsan-announce-check)
  target_compile_options(rtsan-announce-check PRIVATE -Wno-function-effects)
  add_test(NAME rtsan-announce-check.audio COMMAND rtsan-announce-check audio)
  set_tests_properties(rtsan-announce-check.audio PROPERTIES WILL_FAIL TRUE)
  add_test(NAME rtsan-announce-check.audio-bridge COMMAND rtsan-announce-check audio-bridge)
  set_tests_properties(rtsan-announce-check.audio-bridge PROPERTIES WILL_FAIL TRUE)
  add_test(NAME rtsan-announce-check.audio-bridge-end COMMAND rtsan-announce-check audio-bridge-end)
  set_tests_properties(rtsan-announce-check.audio-bridge-end PROPERTIES WILL_FAIL TRUE)
  add_test(NAME rtsan-announce-check.cold COMMAND rtsan-announce-check cold)
endif()
