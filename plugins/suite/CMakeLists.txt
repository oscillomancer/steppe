# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# The suite layer (ADR-0032): what turns a product of the plugin template
# into a member of the suite. Here: the DSP script runtime and the script
# compiler. Both build without the rest of the monorepo, as the player and
# source exports need.

set(SUPPLE_TEMPLATE_FRAMEWORK ${CMAKE_SOURCE_DIR}/plugins/template/framework)

# The runtime: <supple/script.h> and its implementation over the framework's
# DSP library.
add_library(script-runtime STATIC runtime/src/runtime.cpp)
target_include_directories(script-runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/runtime/include)
target_include_directories(script-runtime SYSTEM PRIVATE ${SUPPLE_TEMPLATE_FRAMEWORK})
set_target_properties(script-runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
supple_target_defaults(script-runtime)
add_library(supple::script-runtime ALIAS script-runtime)

# The compiler: front end and MIR code generation (supple/script_compiler.hpp).
add_library(script-compiler STATIC
  compiler/src/lexer.cpp
  compiler/src/parser.cpp
  compiler/src/builtins.cpp
  compiler/src/sema.cpp
  compiler/src/sema_expr.cpp
  compiler/src/codegen.cpp
  compiler/src/compiler.cpp)
target_include_directories(script-compiler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/compiler/include
                                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compiler/src)
target_link_libraries(script-compiler PUBLIC supple::script-runtime PRIVATE supple::mir supple::fast-float)
set_target_properties(script-compiler PROPERTIES POSITION_INDEPENDENT_CODE ON)
supple_target_defaults(script-compiler)
add_library(supple::script-compiler ALIAS script-compiler)

# The framework's SDK-free core, as the template has it, for the suite's own
# tests; a plugin built from the template compiles its own. Vendored code:
# its warnings are the template's to keep clean.
add_library(suite-framework-core STATIC ${SUPPLE_TEMPLATE_FRAMEWORK}/core/Param.cpp
                                        ${SUPPLE_TEMPLATE_FRAMEWORK}/core/Definition.cpp)
target_include_directories(suite-framework-core SYSTEM PUBLIC ${SUPPLE_TEMPLATE_FRAMEWORK})
target_compile_definitions(suite-framework-core PUBLIC SUPPLE_PRODUCT_RUNTIME=1)
target_compile_features(suite-framework-core PUBLIC cxx_std_20)
target_compile_options(suite-framework-core PRIVATE -w ${_supple_sanitize_flags})
set_target_properties(suite-framework-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The semantic layer's documents: JSON, meaning, patches, schemas.
# Self-contained, as exports carry it. (The layer itself, semantic_layer.cpp,
# is built with the plugin that uses it.)
add_library(suite-semantic STATIC semantic/src/json.cpp semantic/src/semantics.cpp)
target_include_directories(suite-semantic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/semantic/include)
target_link_libraries(suite-semantic PUBLIC suite-framework-core PRIVATE supple::core-base)
set_target_properties(suite-semantic PROPERTIES POSITION_INDEPENDENT_CODE ON)
supple_target_defaults(suite-semantic)
add_library(supple::suite-semantic ALIAS suite-semantic)

if(SUPPLE_BUILD_TESTS)
  add_executable(suite-semantic.tests semantic/tests/test_json.cpp semantic/tests/test_semantics.cpp)
  target_link_libraries(suite-semantic.tests PRIVATE supple::suite-semantic supple::doctest-main)
  supple_target_defaults(suite-semantic.tests)
  add_test(NAME suite-semantic.tests COMMAND suite-semantic.tests)
endif()

if(SUPPLE_BUILD_TESTS)
  add_executable(script-compiler.tests compiler/tests/test_compiler.cpp)
  target_include_directories(script-compiler.tests PRIVATE compiler/tests)
  # The framework's DelayLine, the reference the runtime's line must match.
  target_include_directories(script-compiler.tests SYSTEM PRIVATE ${SUPPLE_TEMPLATE_FRAMEWORK})
  target_link_libraries(script-compiler.tests PRIVATE supple::script-compiler supple::doctest-main)
  if(WIN32)
    target_link_libraries(script-compiler.tests PRIVATE psapi)  # QueryWorkingSetEx
  endif()
  target_compile_definitions(script-compiler.tests PRIVATE
                             SUPPLE_SKILL_GUIDES="${PROJECT_SOURCE_DIR}/skill/guides")
  supple_target_defaults(script-compiler.tests)
  add_test(NAME script-compiler.tests COMMAND script-compiler.tests)
  set_tests_properties(script-compiler.tests PROPERTIES LABELS "script-compiler")
endif()

if(SUPPLE_BUILD_TESTS)
  # The examples compiled as C++, as a source export builds them: warnings
  # are the script author's business (not -Werror), signed overflow wraps.
  add_executable(script-examples.tests compiler/tests/test_examples.cpp)
  target_include_directories(script-examples.tests PRIVATE compiler/tests examples)
  target_link_libraries(script-examples.tests PRIVATE supple::script-compiler supple::doctest-main)
  target_compile_features(script-examples.tests PRIVATE cxx_std_20)
  supple_compile_options(script-examples.tests PRIVATE -ffp-contract=off -fwrapv ${_supple_sanitize_flags})
  target_link_options(script-examples.tests PRIVATE ${_supple_sanitize_flags})
  target_compile_definitions(script-examples.tests PRIVATE SUPPLE_SCRIPT_EXAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/examples")
  add_test(NAME script-examples.tests COMMAND script-examples.tests)
  set_tests_properties(script-examples.tests PROPERTIES LABELS "script-compiler")
  # The skill's copy of the examples (skill/generated/dsp-examples.md).
  add_test(NAME script-examples.guide-drift
    COMMAND ${CMAKE_COMMAND} -DEXAMPLES=${CMAKE_CURRENT_SOURCE_DIR}/examples
            -DOUT=${PROJECT_SOURCE_DIR}/skill/generated/dsp-examples.md -DCHECK=ON
            -P ${PROJECT_SOURCE_DIR}/tools/gen/dsp_examples.cmake)
endif()

# What dsp.export's source exports are made of (host/dsp-commands), staged
# beside the stock plugins as the player is: the template's framework and
# root files, the suite without its compiler, and the files an export's own
# part starts from (export/). Copied afresh at every build, so nothing
# removed here lingers there.
set(kit ${CMAKE_BINARY_DIR}/plugins/script-export)
set(kit_template ${CMAKE_SOURCE_DIR}/plugins/template)
add_custom_target(script-export-kit ALL
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${kit}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${kit}/template ${kit}/suite
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${kit_template}/framework ${kit}/template/framework
  COMMAND ${CMAKE_COMMAND} -E copy ${kit_template}/CMakeLists.txt ${kit_template}/CMakePresets.json
          ${kit_template}/.gitignore ${kit}/template
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/runtime ${kit}/suite/runtime
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/semantic/include
          ${kit}/suite/semantic/include
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/semantic/src
          ${kit}/suite/semantic/src
  COMMAND ${CMAKE_COMMAND} -E make_directory ${kit}/suite/base/include/supple/base ${kit}/suite/base/src
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/libs/core-base/include/supple/base/chars.h
          ${kit}/suite/base/include/supple/base
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/libs/core-base/src/chars.cpp ${kit}/suite/base/src
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE ${kit}/suite/base
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/third_party/fast_float
          ${kit}/suite/third_party/fast_float
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/glue ${kit}/suite/glue
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/tools ${kit}/suite/tools
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/suite.cmake
          ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt ${kit}/suite
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/export ${kit}/files
  COMMENT "Staging the script export kit"
  VERBATIM)
