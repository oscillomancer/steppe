# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
add_library(core-json-fuzzlib STATIC
  ${PROJECT_SOURCE_DIR}/libs/core-json/src/value.cpp
  ${PROJECT_SOURCE_DIR}/libs/core-json/src/parse.cpp
  ${PROJECT_SOURCE_DIR}/libs/core-json/src/write.cpp
  ${PROJECT_SOURCE_DIR}/libs/core-json/src/pointer.cpp
  ${PROJECT_SOURCE_DIR}/libs/core-json/src/merge.cpp)
target_include_directories(core-json-fuzzlib PUBLIC ${PROJECT_SOURCE_DIR}/libs/core-json/include)
target_link_libraries(core-json-fuzzlib PUBLIC supple::core-base)
target_compile_options(core-json-fuzzlib PRIVATE -fsanitize=fuzzer-no-link,address)
supple_target_defaults(core-json-fuzzlib)
add_executable(core-json.fuzz core_json_fuzz.cpp)
target_link_libraries(core-json.fuzz PRIVATE core-json-fuzzlib)
target_compile_options(core-json.fuzz PRIVATE -fsanitize=fuzzer,address)
target_link_options(core-json.fuzz PRIVATE -fsanitize=fuzzer,address)
supple_target_defaults(core-json.fuzz)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/corpus DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME core-json.fuzz-smoke COMMAND core-json.fuzz -runs=100 ${CMAKE_CURRENT_BINARY_DIR}/corpus)
set_tests_properties(core-json.fuzz-smoke PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")

add_library(command-bus-fuzzlib STATIC
  ${PROJECT_SOURCE_DIR}/libs/command-bus/src/bus.cpp
  ${PROJECT_SOURCE_DIR}/libs/command-bus/src/jobs.cpp)
target_include_directories(command-bus-fuzzlib PUBLIC ${PROJECT_SOURCE_DIR}/libs/command-bus/include)
target_link_libraries(command-bus-fuzzlib PUBLIC supple::core-schema supple::product-info)
target_compile_options(command-bus-fuzzlib PRIVATE -fsanitize=fuzzer-no-link,address)
supple_target_defaults(command-bus-fuzzlib)
add_executable(command-bus.fuzz command_bus_fuzz.cpp)
target_link_libraries(command-bus.fuzz PRIVATE command-bus-fuzzlib)
target_compile_options(command-bus.fuzz PRIVATE -fsanitize=fuzzer,address)
target_link_options(command-bus.fuzz PRIVATE -fsanitize=fuzzer,address)
supple_target_defaults(command-bus.fuzz)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/bus-corpus DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME command-bus.fuzz-smoke
  COMMAND command-bus.fuzz -runs=100 ${CMAKE_CURRENT_BINARY_DIR}/bus-corpus)
set_tests_properties(command-bus.fuzz-smoke PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")

add_library(notation-fuzzlib STATIC ${PROJECT_SOURCE_DIR}/libs/notation/src/notation.cpp)
target_include_directories(notation-fuzzlib PUBLIC ${PROJECT_SOURCE_DIR}/libs/notation/include
  PRIVATE ${PROJECT_SOURCE_DIR}/libs/notation/src)
target_link_libraries(notation-fuzzlib PUBLIC supple::midi supple::core-base)
target_compile_options(notation-fuzzlib PRIVATE -fsanitize=fuzzer-no-link,address)
supple_target_defaults(notation-fuzzlib)
add_executable(notation.fuzz notation_fuzz.cpp)
target_link_libraries(notation.fuzz PRIVATE notation-fuzzlib)
target_compile_options(notation.fuzz PRIVATE -fsanitize=fuzzer,address)
target_link_options(notation.fuzz PRIVATE -fsanitize=fuzzer,address)
supple_target_defaults(notation.fuzz)
file(COPY ${PROJECT_SOURCE_DIR}/libs/notation/tests/corpus/valid/
  DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/notation-corpus FILES_MATCHING PATTERN "*.note")
add_test(NAME notation.fuzz-smoke COMMAND notation.fuzz -runs=100
  ${CMAKE_CURRENT_BINARY_DIR}/notation-corpus)
set_tests_properties(notation.fuzz-smoke PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0:alloc_dealloc_mismatch=0")

# Coverage instrumentation: the *-fuzzlib targets above compile their own
# sources with -fsanitize=fuzzer-no-link, but every library they or the
# project/history targets link from the ordinary tree (core-schema, engine,
# model, ...) gets only SUPPLE_SANITIZE's flags, so libFuzzer sees no edges in
# them. A campaign configures the whole fuzz tree with
# -fsanitize=fuzzer-no-link in CMAKE_C_FLAGS/CMAKE_CXX_FLAGS (README.md).
if(NOT CMAKE_CXX_FLAGS MATCHES "fuzzer-no-link")
  message(WARNING "Fuzz targets link libraries without coverage instrumentation; "
    "configure with -DCMAKE_CXX_FLAGS=-fsanitize=fuzzer-no-link "
    "-DCMAKE_C_FLAGS=-fsanitize=fuzzer-no-link for a coverage-guided campaign "
    "(tests/fuzz/README.md). The smoke tests are unaffected.")
endif()

# The project target calls the exact parse/validate path used by project.open.
# The history target calls the real EngineApp command catalogue and saves/opens
# its final session. Both link the engine/model libraries built by this tree.
foreach(name IN ITEMS project history)
  add_executable(${name}.fuzz ${name}_fuzz.cpp)
  target_link_libraries(${name}.fuzz PRIVATE supple::engine)
  target_compile_options(${name}.fuzz PRIVATE -fsanitize=fuzzer,address)
  target_link_options(${name}.fuzz PRIVATE -fsanitize=fuzzer,address)
  supple_target_defaults(${name}.fuzz)
endforeach()
target_compile_definitions(history.fuzz PRIVATE FIXTURE_CLAP_PATH="$<TARGET_FILE:clap-host-fixtures>")
add_dependencies(history.fuzz clap-host-fixtures)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/project-corpus DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/history-corpus DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME project.fuzz-smoke COMMAND project.fuzz -runs=100 -timeout=30
  ${CMAKE_CURRENT_BINARY_DIR}/project-corpus)
add_test(NAME history.fuzz-smoke COMMAND history.fuzz -runs=100
  ${CMAKE_CURRENT_BINARY_DIR}/history-corpus)
# libstdc++'s stable_sort temporary buffer uses get_temporary_buffer's
# operator new/free pair; Clang ASan flags that library implementation.
set_tests_properties(project.fuzz-smoke history.fuzz-smoke PROPERTIES
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0:alloc_dealloc_mismatch=0")
