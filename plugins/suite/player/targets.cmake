# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# The suite in a player build (the folder above this one, Steppe's
# plugins/suite: suite.cmake), and the script compiler with MIR, whose
# sources are in SUPPLE_MIR_DIR (Steppe's third_party/mir-1.0.0).

get_filename_component(SUPPLE_SUITE "${SUPPLE_PRODUCT}/.." ABSOLUTE)
include(${SUPPLE_SUITE}/suite.cmake)
set(SUPPLE_MIR_DIR "${SUPPLE_SUITE}/../../third_party/mir-1.0.0" CACHE PATH "MIR 1.0.0's sources")

add_library(suite-mir STATIC ${SUPPLE_MIR_DIR}/mir.c ${SUPPLE_MIR_DIR}/mir-gen.c)
target_include_directories(suite-mir SYSTEM PUBLIC ${SUPPLE_MIR_DIR})
set_target_properties(suite-mir PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_options(suite-mir PRIVATE -w -fsigned-char)
supple_vendored(suite-mir)
supple_sanitize(suite-mir)
if (SUPPLE_SANITIZE MATCHES "address")
    # MIR's string hash reads unaligned words, which x86-64 and AArch64 allow.
    target_compile_options(suite-mir PRIVATE -fno-sanitize=alignment)
endif()

add_library(suite-compiler STATIC
    ${SUPPLE_SUITE}/compiler/src/lexer.cpp
    ${SUPPLE_SUITE}/compiler/src/parser.cpp
    ${SUPPLE_SUITE}/compiler/src/builtins.cpp
    ${SUPPLE_SUITE}/compiler/src/sema.cpp
    ${SUPPLE_SUITE}/compiler/src/sema_expr.cpp
    ${SUPPLE_SUITE}/compiler/src/codegen.cpp
    ${SUPPLE_SUITE}/compiler/src/compiler.cpp)
target_include_directories(suite-compiler PUBLIC ${SUPPLE_SUITE}/compiler/include
                                          PRIVATE ${SUPPLE_SUITE}/compiler/src)
target_include_directories(suite-compiler SYSTEM PRIVATE ${SUPPLE_FRAMEWORK})
target_include_directories(suite-compiler SYSTEM PRIVATE ${suite_fast_float}/include)
target_link_libraries(suite-compiler PUBLIC suite-runtime PRIVATE suite-mir)
supple_defaults(suite-compiler)

# dladdr (product/Catalog.cpp), in libdl before glibc 2.34.
target_link_libraries(product-core PUBLIC suite-compiler ${CMAKE_DL_LIBS})

if (SUPPLE_BUILD_TESTS AND COMMAND supple_test)
    # Scripts the tests play (SUPPLE_SCRIPT_LIBRARY): host-smoke and the
    # VST3 checks the one with the player's own ID, the rest every one.
    set(player_smoke ${SUPPLE_PRODUCT}/test/smoke)
    if(WIN32)
        set(player_library "${player_smoke}\\;${SUPPLE_PRODUCT}/test/library")
    else()
        set(player_library "${player_smoke}:${SUPPLE_PRODUCT}/test/library")
    endif()
    if (TARGET processor-tests)
        target_include_directories(processor-tests PRIVATE ${SUPPLE_SUITE}/compiler/tests ${SUPPLE_SUITE}/examples)
        target_compile_definitions(processor-tests PRIVATE SUPPLE_SCRIPT_EXAMPLES="${SUPPLE_SUITE}/examples")
        if(EXISTS "${SUPPLE_SUITE}/../marram/script/marram.cpp")
            target_compile_definitions(processor-tests PRIVATE
                SUPPLE_MARRAM_SCRIPT="${SUPPLE_SUITE}/../marram/script/marram.cpp")
        endif()
        # The examples compiled as C++, as source exports build them.
        if (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            target_compile_options(processor-tests PRIVATE /clang:-ffp-contract=off /clang:-fwrapv)
        else()
            target_compile_options(processor-tests PRIVATE -ffp-contract=off -fwrapv)
        endif()
        set_tests_properties(processor-tests PROPERTIES ENVIRONMENT "SUPPLE_SCRIPT_LIBRARY=${player_library}")
    endif()
    foreach(test host-smoke-2ch host-smoke-1ch vst3-unload exports-clap exports-vst3 x11-embedding
                 x11-embedding-overlapped mac-embedding win32-embedding win32-embedding-no-host-timer
                 win32-module-ownership)
        if (TEST ${test})
            set_tests_properties(${test} PROPERTIES ENVIRONMENT "SUPPLE_SCRIPT_LIBRARY=${player_smoke}")
        endif()
    endforeach()
    if (SUPPLE_BUILD_PLUGIN)
        supple_test(player-plugin-tests SOURCES ${SUPPLE_PRODUCT}/test/plugin_test.cpp LINK product-impl)
        set_tests_properties(player-plugin-tests PROPERTIES ENVIRONMENT "SUPPLE_SCRIPT_LIBRARY=${player_library}")
    endif()
endif()
