# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
supple_add_library(core-base
  SOURCES src/paths.cpp src/waitable.cpp src/rt.cpp src/main_thread_wake.cpp src/thread.cpp src/result.cpp src/chars.cpp src/units.cpp src/closest.cpp src/files.cpp src/sha256.cpp src/md5.cpp
  DEPS supple::product-info PRIVATE_DEPS supple::fast-float)

if(WIN32)
  target_link_libraries(core-base PRIVATE Synchronization psapi)
endif()

supple_add_tests(core-base SOURCES tests/test_paths.cpp tests/test_waitable.cpp tests/test_result.cpp tests/test_chars.cpp tests/test_rt_primitives.cpp tests/test_main_thread_wake.cpp tests/test_thread.cpp tests/test_files.cpp tests/test_environment.cpp tests/test_sha256.cpp tests/test_md5.cpp LABELS rt)

if(SUPPLE_BUILD_TESTS)
  add_executable(core-base-stop.tests tests/test_stop.cpp)
  target_link_libraries(core-base-stop.tests PRIVATE supple::core-base supple::doctest-main)
  supple_target_defaults(core-base-stop.tests)
  add_test(NAME core-base-stop.tests COMMAND core-base-stop.tests)
  add_executable(core-base-stop-fallback.tests tests/test_stop.cpp)
  target_link_libraries(core-base-stop-fallback.tests PRIVATE supple::core-base supple::doctest-main)
  target_compile_definitions(core-base-stop-fallback.tests PRIVATE SUPPLE_TEST_FALLBACK_STOP=1)
  supple_target_defaults(core-base-stop-fallback.tests)
  add_test(NAME core-base-stop-fallback.tests COMMAND core-base-stop-fallback.tests)
endif()
