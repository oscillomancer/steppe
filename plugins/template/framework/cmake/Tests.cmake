# Tests, tools and probes. See BUILDING.md for what each one proves.

enable_testing()
find_package(Threads REQUIRED)

function(supple_test name)
    cmake_parse_arguments(T "" "" "SOURCES;LINK" ${ARGN})
    add_executable(${name} ${T_SOURCES})
    target_link_libraries(${name} PRIVATE ${T_LINK} Threads::Threads)
    target_include_directories(${name} PRIVATE ${SUPPLE_FRAMEWORK}/test)
    supple_defaults(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# --- SDK-free -----------------------------------------------------------------------------

add_test(NAME bin2c
    COMMAND ${CMAKE_COMMAND} -DFRAMEWORK=${SUPPLE_FRAMEWORK}
        -DWORK=${CMAKE_BINARY_DIR}/bin2c-test
        -P ${SUPPLE_FRAMEWORK}/cmake/test/Bin2c.cmake)

add_test(NAME sanitizer-config
    COMMAND ${CMAKE_COMMAND} -DFRAMEWORK=${SUPPLE_FRAMEWORK}
        -P ${SUPPLE_FRAMEWORK}/cmake/test/Sanitizer.cmake)

add_test(NAME new-product
    COMMAND ${CMAKE_COMMAND} -DFRAMEWORK=${SUPPLE_FRAMEWORK}
        -DWORK=${CMAKE_BINARY_DIR}/new-product-test
        -P ${SUPPLE_FRAMEWORK}/cmake/test/NewProduct.cmake)

add_test(NAME patched-source
    COMMAND ${CMAKE_COMMAND} -DFRAMEWORK=${SUPPLE_FRAMEWORK}
        -DWORK=${CMAKE_BINARY_DIR}/patched-source-test
        -P ${SUPPLE_FRAMEWORK}/cmake/test/PatchedSource.cmake)

add_test(NAME framework-update
    COMMAND ${CMAKE_COMMAND} -DFRAMEWORK=${SUPPLE_FRAMEWORK}
        -DWORK=${CMAKE_BINARY_DIR}/framework-update-test
        -P ${SUPPLE_FRAMEWORK}/test/update_framework_test.cmake)
if (UNIX AND EXISTS "${SUPPLE_ROOT}/.github/workflows/build.yml")
    add_test(NAME ci-product-names COMMAND ${CMAKE_COMMAND}
        -DWORKFLOW=${SUPPLE_ROOT}/.github/workflows/build.yml
        -DWORK=${CMAKE_BINARY_DIR}/ci-names-test
        -P ${SUPPLE_FRAMEWORK}/cmake/test/CiNames.cmake)
endif()
if (UNIX AND EXISTS "${SUPPLE_ROOT}/.github/scripts/clap-probe-mac.sh")
    add_test(NAME ci-probe-checker COMMAND bash ${SUPPLE_FRAMEWORK}/test/ci_probe_test.sh
        ${SUPPLE_ROOT}/.github/scripts/clap-probe-mac.sh)
endif()

# Tests and tools that read the product's tables as constants (product/
# Params.h, ProductInfo.h). A run-time product (PRODUCT_RUNTIME ON) has none:
# its own tests cover the plugins its catalog makes, and host-smoke and the
# validators drive them through CLAP.
set(supple_tables ON)
if (PRODUCT_RUNTIME)
    set(supple_tables OFF)
endif()

if (supple_tables)
    supple_test(core-tests SOURCES ${SUPPLE_FRAMEWORK}/test/core_test.cpp LINK product-core)
endif()
supple_test(dsp-tests SOURCES ${SUPPLE_FRAMEWORK}/test/dsp_test.cpp LINK product-core)
if (product_test_sources)
    supple_test(processor-tests SOURCES ${product_test_sources} LINK product-core)
endif()

if (supple_tables)
# The product's cost against its stored baseline (tools/benchmark.cpp);
# a timing means something only in an optimised build without sanitizers.
add_executable(benchmark ${SUPPLE_FRAMEWORK}/tools/benchmark.cpp)
target_link_libraries(benchmark PRIVATE product-core)
target_include_directories(benchmark PRIVATE ${SUPPLE_FRAMEWORK}/test)
target_compile_definitions(benchmark PRIVATE SUPPLE_BENCHMARK_FILE="${SUPPLE_PRODUCT}/benchmark.txt")
supple_defaults(benchmark)
if (CMAKE_BUILD_TYPE STREQUAL "Release" AND SUPPLE_SANITIZE STREQUAL "" AND NOT SUPPLE_FUZZ)
    add_test(NAME benchmark COMMAND benchmark)
    set_tests_properties(benchmark PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
endif()

add_executable(render-cli ${SUPPLE_FRAMEWORK}/tools/render_cli.cpp)
target_link_libraries(render-cli PRIVATE product-core)
supple_defaults(render-cli)
endif()

# --- editor -------------------------------------------------------------------------------

if (SUPPLE_BUILD_GUI AND supple_tables)
    supple_test(ui-tests SOURCES ${SUPPLE_FRAMEWORK}/test/ui_test.cpp LINK product-gui)
    if (NOT product_editor_sources)
        target_compile_definitions(ui-tests PRIVATE SUPPLE_AUTO_EDITOR=1)
    endif()

    add_executable(gui-preview ${SUPPLE_FRAMEWORK}/tools/gui_preview.cpp)
    target_link_libraries(gui-preview PRIVATE ${SUPPLE_GL_GUI})
    target_compile_definitions(gui-preview PRIVATE SUPPLE_PREVIEWS_FILE="${SUPPLE_PRODUCT}/previews.txt")
    supple_defaults(gui-preview)
    # Every named scenario (product/previews.txt) renders, into
    # build/<preset>/previews. Skipped without a display.
    add_test(NAME previews COMMAND gui-preview --all ${CMAKE_BINARY_DIR}/previews)
    set_tests_properties(previews PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
    # Check the exit status as well as the diagnostic: a missing display must
    # not accidentally make this negative test pass.
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/preview-invalid.cmake" CONTENT [=[
# Scenarios that lead back to themselves are refused, before any window.
file(WRITE "${CMAKE_CURRENT_LIST_DIR}/preview-loops.txt" "loop --scenario loop\nping --scenario pong\npong --scenario ping\n")
foreach(name loop ping)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "SUPPLE_PREVIEWS_FILE=${CMAKE_CURRENT_LIST_DIR}/preview-loops.txt"
        "$<TARGET_FILE:gui-preview>" invalid.png --scenario ${name}
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 2 OR NOT err MATCHES "leads back to itself")
        message(FATAL_ERROR "Expected scenario ${name} to be refused, got ${result}: ${out}${err}")
    endif()
endforeach()
if(CMAKE_HOST_UNIX AND NOT CMAKE_HOST_APPLE AND "$ENV{DISPLAY}" STREQUAL "")
    message("No display: skipped")
    return()
endif()
execute_process(COMMAND "$<TARGET_FILE:gui-preview>" invalid.png --seconds 0 --hover __missing_preview_control__
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(result EQUAL 77)
    message("No display: skipped")
    return()
elseif(NOT result EQUAL 2 OR NOT err MATCHES "no control for")
    message(FATAL_ERROR "Expected an unknown-control error, got ${result}: ${out}${err}")
endif()
# A menu asked of a control that opens none (the starter's Mix knob) is an
# error, not a picture of the knob.
execute_process(COMMAND "$<TARGET_FILE:gui-preview>" invalid.png --seconds 0 --menu Mix
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(err MATCHES "no control for")
    message("(no control named Mix in this product)")
elseif(NOT result EQUAL 2 OR NOT err MATCHES "has no menu")
    message(FATAL_ERROR "Expected a no-menu error, got ${result}: ${out}${err}")
endif()
]=])
    add_test(NAME preview-invalid-control COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/preview-invalid.cmake")
    set_tests_properties(preview-invalid-control PROPERTIES SKIP_REGULAR_EXPRESSION "No display: skipped" RUN_SERIAL TRUE)

endif()

# --- plugin -------------------------------------------------------------------------------

if (SUPPLE_BUILD_PLUGIN)
    # The release archive as product-package makes it -- every format, the
    # license, notices and install notes, bundles signed ad hoc on macOS --
    # with no installer, identity or notarization.
    add_test(NAME package COMMAND ${SUPPLE_PACKAGE_COMMAND} -DOUT=${CMAKE_BINARY_DIR}/package-test -DTEST=ON
        -P ${SUPPLE_FRAMEWORK}/cmake/Package.cmake)
    # An ARM64 Windows build is refused before anything is staged.
    add_test(NAME package-windows-arm64 COMMAND ${SUPPLE_PACKAGE_COMMAND} -DPLATFORM=windows-arm64 -DINSTALLER=ON
        -DOUT=${CMAKE_BINARY_DIR}/package-arm64-test -P ${SUPPLE_FRAMEWORK}/cmake/Package.cmake)
    set_tests_properties(package-windows-arm64 PROPERTIES
        PASS_REGULAR_EXPRESSION "x64 only; windows-arm64 is not supported")

    supple_test(probe-audio-tests SOURCES ${SUPPLE_FRAMEWORK}/test/probe_audio_test.cpp LINK product-core clap)
    if (supple_tables)
    # The CLAP surface, in process: a scripted host with real threads.
    supple_test(plugin-tests SOURCES ${SUPPLE_FRAMEWORK}/test/plugin_test.cpp LINK product-impl)

    # Damaged state through CLAP's state calls (test/state_fuzz.cpp), from a
    # fixed seed; with SUPPLE_FUZZ (the fuzz preset), libFuzzer's version too,
    # run for a minute from the same states.
    supple_test(state-fuzz SOURCES ${SUPPLE_FRAMEWORK}/test/state_fuzz.cpp LINK product-impl)
    endif()
    if (SUPPLE_FUZZ AND supple_tables)
        add_executable(state-fuzzer ${SUPPLE_FRAMEWORK}/test/state_fuzz.cpp)
        target_link_libraries(state-fuzzer PRIVATE product-impl)
        target_compile_definitions(state-fuzzer PRIVATE SUPPLE_LIBFUZZER=1)
        supple_defaults(state-fuzzer)
        target_link_options(state-fuzzer PRIVATE -fsanitize=fuzzer)
        set(corpus ${CMAKE_BINARY_DIR}/state-corpus)
        add_test(NAME state-corpus COMMAND state-fuzz --seeds ${corpus})
        add_test(NAME state-fuzzer COMMAND state-fuzzer ${corpus} -max_total_time=60 -timeout=20 -rss_limit_mb=4096
            -print_final_stats=1)
        set_tests_properties(state-corpus PROPERTIES FIXTURES_SETUP state-corpus)
        set_tests_properties(state-fuzzer PROPERTIES FIXTURES_REQUIRED state-corpus TIMEOUT 300)
    endif()

    # A host that loads the built .clap from disk, once per bus layout.
    add_executable(host-smoke ${SUPPLE_FRAMEWORK}/test/host_smoke.cpp)
    target_link_libraries(host-smoke PRIVATE clap ${CMAKE_DL_LIBS})
    target_compile_definitions(host-smoke PRIVATE
        HOST_SMOKE_PLUGIN_ID="${PRODUCT_ID}"
        HOST_SMOKE_PLAIN_PARAM="${PRODUCT_SMOKE_PLAIN_PARAM}")
    if (PRODUCT_SMOKE_LATENCY_PARAM)
        target_compile_definitions(host-smoke PRIVATE HOST_SMOKE_LATENCY_PARAM="${PRODUCT_SMOKE_LATENCY_PARAM}")
    endif()
    if (PRODUCT_TRANSPARENT)
        target_compile_definitions(host-smoke PRIVATE HOST_SMOKE_TRANSPARENT=1)
    endif()
    supple_defaults(host-smoke)
    # Once per layout (cmake/Capabilities.cmake): host-smoke-2ch, -5.1, -ambi1,
    # or -notes for a note effect.
    foreach(layout ${SUPPLE_LAYOUTS})
        if (layout MATCHES "^[0-9]+$")
            set(name ${layout}ch)
        elseif (layout STREQUAL "none")
            set(name notes)
        else()
            set(name ${layout})
        endif()
        add_test(NAME host-smoke-${name}
            COMMAND host-smoke $<TARGET_FILE:product-plugin_clap> --layout ${layout})
    endforeach()

    # Each wrapper patch, still applied to the pinned checkout.
    get_property(patches GLOBAL PROPERTY SUPPLE_PATCHES)
    foreach(entry ${patches})
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 name)
        list(GET parts 1 dir)
        list(GET parts 2 file)
        add_test(NAME patch-${name} COMMAND ${GIT_EXECUTABLE} -C "${dir}" apply --reverse --check "${file}")
    endforeach()

    # A minimal GUI host: macOS window embedding, timers, resize, teardown.
    add_executable(clap-probe ${SUPPLE_FRAMEWORK}/tools/clap_probe.cpp)
    target_link_libraries(clap-probe PRIVATE clap ${CMAKE_DL_LIBS})
    supple_defaults(clap-probe)
    if (APPLE)
        set_source_files_properties(${SUPPLE_FRAMEWORK}/tools/clap_probe.cpp PROPERTIES LANGUAGE OBJCXX)
        target_compile_options(clap-probe PRIVATE -fno-objc-arc)
        target_link_libraries(clap-probe PRIVATE "-framework Cocoa")
    endif()

    if (UNIX AND NOT APPLE)
        foreach(spec "clap:clap_entry" "vst3:GetPluginFactory,ModuleEntry,ModuleExit,clap_entry")
            string(REPLACE ":" ";" parts "${spec}")
            list(GET parts 0 format)
            list(GET parts 1 expected)
            if (TARGET product-plugin_${format})
                add_test(NAME exports-${format}
                    COMMAND ${CMAKE_COMMAND} -DMODULE=$<TARGET_FILE:product-plugin_${format}> "-DEXPECTED=${expected}"
                            -P ${SUPPLE_FRAMEWORK}/cmake/CheckExports.cmake)
            endif()
        endforeach()
    endif()

    if (UNIX AND NOT APPLE AND TARGET product-plugin_vst3)
        add_executable(vst3-unload-probe ${SUPPLE_FRAMEWORK}/test/vst3_unload_probe.cpp)
        target_link_libraries(vst3-unload-probe PRIVATE ${CMAKE_DL_LIBS})
        supple_defaults(vst3-unload-probe)
        add_test(NAME vst3-unload COMMAND vst3-unload-probe $<TARGET_FILE:product-plugin_vst3>)

        # The VST3 against the CLAP it wraps: buses, layouts, MIDI mapping, and
        # the same performance rendered through both. A host needs the SDK's
        # interface IDs and nothing else of it.
        set(vst3_sdk "${CPM_PACKAGE_vst3sdk_SOURCE_DIR}")
        if (NOT vst3_sdk)
            set(vst3_sdk "${VST3_SDK_ROOT}")  # vendored
        endif()
        if (vst3_sdk AND EXISTS "${vst3_sdk}/pluginterfaces/base/funknown.cpp" AND supple_tables)
            add_library(supple-vst3-iids OBJECT ${vst3_sdk}/pluginterfaces/base/funknown.cpp
                ${vst3_sdk}/pluginterfaces/base/coreiids.cpp ${vst3_sdk}/public.sdk/source/vst/vstinitiids.cpp)
            target_include_directories(supple-vst3-iids SYSTEM PRIVATE ${vst3_sdk})
            target_compile_options(supple-vst3-iids PRIVATE -w)
            supple_sanitize(supple-vst3-iids)
            add_executable(vst3-host-tests ${SUPPLE_FRAMEWORK}/test/vst3_host_test.cpp $<TARGET_OBJECTS:supple-vst3-iids>)
            target_include_directories(vst3-host-tests SYSTEM PRIVATE ${vst3_sdk})
            target_include_directories(vst3-host-tests PRIVATE ${SUPPLE_FRAMEWORK}/test)
            target_link_libraries(vst3-host-tests PRIVATE product-core clap ${CMAKE_DL_LIBS})
            target_compile_definitions(vst3-host-tests PRIVATE VST3_HOST_PLAIN_PARAM="${PRODUCT_SMOKE_PLAIN_PARAM}")
            supple_defaults(vst3-host-tests)
            # The SDK's interfaces have no virtual destructors, by design.
            target_compile_options(vst3-host-tests PRIVATE -Wno-non-virtual-dtor)
            add_test(NAME vst3-host COMMAND vst3-host-tests $<TARGET_FILE:product-plugin_vst3>
                $<TARGET_FILE:product-plugin_clap>)
        endif()
    endif()

    if (UNIX AND NOT APPLE)
        if (SUPPLE_BUILD_GUI)
            # A real X11 parent: embedding, pixels, resize, scale, reopen,
            # teardown while audio runs. Skipped (77) without a display.
            find_package(X11 REQUIRED)
            add_executable(x11-probe ${SUPPLE_FRAMEWORK}/tools/x11_probe.cpp)
            target_link_libraries(x11-probe PRIVATE clap X11::X11 ${CMAKE_DL_LIBS} Threads::Threads)
            supple_defaults(x11-probe)
            add_test(NAME x11-embedding
                COMMAND x11-probe $<TARGET_FILE:product-plugin_clap> ${CMAKE_BINARY_DIR}/x11-probe.ppm)
            set_tests_properties(x11-embedding PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
            # Six at once, their windows on top of each other.
            add_test(NAME x11-embedding-overlapped
                COMMAND bash ${SUPPLE_FRAMEWORK}/test/x11_overlap_test.sh $<TARGET_FILE:x11-probe>
                        $<TARGET_FILE:product-plugin_clap> ${CMAKE_BINARY_DIR}/x11-overlap)
            set_tests_properties(x11-embedding-overlapped PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
        endif()
    endif()

    if (APPLE AND SUPPLE_BUILD_GUI AND EXISTS "${SUPPLE_ROOT}/.github/scripts/clap-probe-mac.sh")
        # A real Cocoa host window, in each host scenario the CI wrapper
        # names and checks: open, resize, size before parenting, reopen, no
        # host timer, teardown while hot, a parent dropped, mono. Each
        # scenario's report and picture land in mac-probe/probe.
        set(mac_probe "${CMAKE_BINARY_DIR}/mac-probe")
        file(MAKE_DIRECTORY "${mac_probe}")
        add_test(NAME mac-embedding
            COMMAND bash ${SUPPLE_ROOT}/.github/scripts/clap-probe-mac.sh "${PRODUCT_NAME}"
                    $<TARGET_FILE:product-plugin_clap> $<TARGET_FILE:clap-probe>
            WORKING_DIRECTORY "${mac_probe}")
        set_tests_properties(mac-embedding PROPERTIES RUN_SERIAL TRUE TIMEOUT 1200)
    endif()

    if (WIN32 AND SUPPLE_BUILD_GUI)
        # A real Win32 parent: embedding, pixels, resize, set_scale(2),
        # hide/show, three create/destroy cycles while audio runs. Needs an
        # OpenGL 2 driver; see the note at the top of win32_probe.cpp.
        add_executable(win32-probe ${SUPPLE_FRAMEWORK}/tools/win32_probe.cpp)
        target_link_libraries(win32-probe PRIVATE clap user32 gdi32 Threads::Threads)
        target_compile_definitions(win32-probe PRIVATE PROBE_PLAIN_PARAM="${PRODUCT_SMOKE_PLAIN_PARAM}")
        supple_defaults(win32-probe)
        add_test(NAME win32-embedding
            COMMAND win32-probe $<TARGET_FILE:product-plugin_clap> ${CMAKE_BINARY_DIR}/win32-probe.ppm)
        # A host without clap.timer-support: the editor's own WM_TIMER
        # fallback must carry it through the same lifecycle.
        add_test(NAME win32-embedding-no-host-timer
            COMMAND win32-probe $<TARGET_FILE:product-plugin_clap> ${CMAKE_BINARY_DIR}/win32-probe-no-timer.ppm
                    --no-host-timer)
        set_tests_properties(win32-embedding win32-embedding-no-host-timer PROPERTIES RUN_SERIAL TRUE)
        if (TARGET product-plugin_vst3)
            add_executable(win32-module-test ${SUPPLE_FRAMEWORK}/test/win32_module_test.cpp)
            target_link_libraries(win32-module-test PRIVATE clap user32)
            supple_defaults(win32-module-test)
            add_test(NAME win32-module-ownership COMMAND win32-module-test
                $<TARGET_FILE:product-plugin_clap> $<TARGET_FILE:product-plugin_vst3>)
            set_tests_properties(win32-module-ownership PROPERTIES RUN_SERIAL TRUE)
        endif()
    endif()

    if (APPLE AND TARGET product-plugin_auv2)
        add_test(NAME auv2-bundle
            COMMAND ${CMAKE_COMMAND} -DBUNDLE=${SUPPLE_ASSETS}/${PRODUCT_NAME}.component
                    -P ${SUPPLE_FRAMEWORK}/cmake/CheckAuBundle.cmake)
        # Automation on one thread while state loads rescan the parameters on
        # another; the tsan preset is what makes it a race check.
        add_executable(au-parameter-race ${SUPPLE_FRAMEWORK}/test/au_parameter_race.cpp)
        target_link_libraries(au-parameter-race PRIVATE "-framework AudioToolbox" "-framework CoreFoundation")
        supple_defaults(au-parameter-race)
        add_dependencies(au-parameter-race product-plugin_auv2)
        add_test(NAME auv2-parameter-race
            COMMAND au-parameter-race ${SUPPLE_ASSETS}/${PRODUCT_NAME}.component)
    endif()

    # free-audio/clap-validator, when installed (cargo install --git
    # https://github.com/free-audio/clap-validator), or pointed to with
    # -DSUPPLE_CLAP_VALIDATOR=/path/to/clap-validator.
    find_program(SUPPLE_CLAP_VALIDATOR clap-validator)
    if (SUPPLE_CLAP_VALIDATOR AND NOT SUPPLE_SANITIZE STREQUAL "")
        # It cannot load a module that expects a sanitizer runtime from its host.
        message(STATUS "clap-validator is not run against a sanitizer build")
    elseif (SUPPLE_CLAP_VALIDATOR)
        if (APPLE)
            set(validated "${SUPPLE_ASSETS}/${PRODUCT_NAME}.clap")
        else()
            set(validated "$<TARGET_FILE:product-plugin_clap>")
        endif()
        set(validator_options --only-failed)
        if (PRODUCT_PRESETS)
            # clap-validator 0.4.1 deadlocks on the second preset of any
            # container -- a plugin location listing two factory presets, as
            # CLAP has them -- in its own metadata receiver (begin_preset holds
            # the lock flush_preset takes). plugin-tests crawls and loads them
            # instead. Drop this when a release fixes it.
            list(APPEND validator_options --exclude "^preset-discovery-(crawl|load)$")
        endif()
        add_test(NAME clap-validator COMMAND ${SUPPLE_CLAP_VALIDATOR} validate ${validator_options} "${validated}")
        set_tests_properties(clap-validator PROPERTIES TIMEOUT 300)
    else()
        message(STATUS "clap-validator not found; its test is not registered")
    endif()
endif()

# What the fuzz preset builds: the state fuzzers above, or nothing when the
# product has none (a run-time product, whose state no tables describe).
add_custom_target(fuzz-tests)
foreach(fuzzer state-fuzz state-fuzzer)
    if (TARGET ${fuzzer})
        add_dependencies(fuzz-tests ${fuzzer})
    endif()
endforeach()
