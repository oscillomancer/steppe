# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
add_subdirectory(common)
add_subdirectory(suite)

if(SUPPLE_BUILD_TESTS)
  add_executable(stock-marram.tests common/tests/marram.cpp)
  target_link_libraries(stock-marram.tests PRIVATE supple::script-runtime supple::doctest-main)
  target_compile_features(stock-marram.tests PRIVATE cxx_std_20)
  supple_compile_options(stock-marram.tests PRIVATE -ffp-contract=off -fwrapv ${_supple_sanitize_flags})
  target_link_options(stock-marram.tests PRIVATE ${_supple_sanitize_flags})
  add_test(NAME stock-marram.tests COMMAND stock-marram.tests)
  set_tests_properties(stock-marram.tests PROPERTIES LABELS rt)
  add_executable(stock-nonfinite.tests common/tests/nonfinite.cpp)
  target_link_libraries(stock-nonfinite.tests PRIVATE supple::script-runtime supple::doctest-main)
  target_compile_features(stock-nonfinite.tests PRIVATE cxx_std_20)
  supple_compile_options(stock-nonfinite.tests PRIVATE -ffp-contract=off -fwrapv ${_supple_sanitize_flags})
  target_link_options(stock-nonfinite.tests PRIVATE ${_supple_sanitize_flags})
  add_test(NAME stock-nonfinite.tests COMMAND stock-nonfinite.tests)
  set_tests_properties(stock-nonfinite.tests PROPERTIES LABELS rt)
  add_subdirectory(drumlin/fit)
endif()

if(SUPPLE_BUILD_PLUGINS)
  include(ExternalProject)
  option(SUPPLE_PLUGIN_VST3 "Also build VST3 versions of the stock plugins" OFF)
  set(VST3_SDK_ROOT "" CACHE PATH "Complete VST3 SDK for opt-in stock-plugin builds")
  # supple_add_template_product(<name> <product> <artifact>): a product of
  # the plugin template at framework 1.16.1 or later, built from the template's
  # tree (plugins/template) with plugins/<product> as its product folder, as
  # an isolated ExternalProject. Its dependencies are this repository's
  # third_party copies, at the template's pins; it makes the CLAP alone
  # unless SUPPLE_PLUGIN_VST3 (with a complete VST3_SDK_ROOT). The CLAP, its
  # license and notices are staged in plugins/clap beside the stock plugins.
  # Its own ctest suite becomes plugin.<name>.ctest.
  function(supple_add_template_product name product artifact)
    set(plugin_renderer_args)
    if(APPLE)
      list(APPEND plugin_renderer_args
           -DFETCHCONTENT_SOURCE_DIR_NANOVG_METAL=${CMAKE_SOURCE_DIR}/third_party/nanovg_metal)
    endif()
    set_property(GLOBAL APPEND PROPERTY SUPPLE_STAGED_PLUGINS "${artifact}")
    set(plugin_build "${CMAKE_BINARY_DIR}/plugins/${name}")
    set(plugin_output_args)
    if(WIN32)
      string(TOUPPER "${CMAKE_BUILD_TYPE}" plugin_config)
      list(APPEND plugin_output_args "-DCMAKE_LIBRARY_OUTPUT_DIRECTORY_${plugin_config}=${plugin_build}/plugins")
    endif()
    # As for the stock plugins: instrumented for tsan and rtsan, whose hosts
    # are, and not for asan.
    if(SUPPLE_SANITIZE STREQUAL "realtime" OR SUPPLE_SANITIZE STREQUAL "thread")
      set(plugin_sanitize ${SUPPLE_SANITIZE})
    else()
      set(plugin_sanitize "")
    endif()
    set(plugin_build_targets)
    if(SUPPLE_PACKAGE_ONLY)
      # Select existing template targets; leave its shared files unchanged.
      # In particular, macOS packages ship CLAP alone, not AUv2 wrappers.
      list(APPEND plugin_build_targets --target product-plugin_clap product-notices)
    endif()
    set(tp "${CMAKE_SOURCE_DIR}/third_party")
    ExternalProject_Add(plugin-${name}
      SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/template"
      BINARY_DIR "${plugin_build}"
      DOWNLOAD_COMMAND ""
      UPDATE_COMMAND ""
      BUILD_ALWAYS TRUE
      INSTALL_COMMAND ""
      CMAKE_GENERATOR Ninja
      CMAKE_ARGS
        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}
        -DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        "-DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}"
        "-DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
        "-DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
        "-DCMAKE_SHARED_LINKER_FLAGS=${CMAKE_SHARED_LINKER_FLAGS}"
        "-DCMAKE_MODULE_LINKER_FLAGS=${CMAKE_MODULE_LINKER_FLAGS}"
        ${plugin_output_args}
        -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
        -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DSUPPLE_PRODUCT_DIR=${CMAKE_CURRENT_SOURCE_DIR}/${product}
        -DSUPPLE_BUILD_GUI=${SUPPLE_BUILD_GUI}
        ${plugin_renderer_args}
        -DSUPPLE_BUILD_TESTS=${SUPPLE_BUILD_TESTS}
        -DSUPPLE_BUILD_VST3=${SUPPLE_PLUGIN_VST3}
        -DVST3_SDK_ROOT=${VST3_SDK_ROOT}
        -DAUDIOUNIT_SDK_ROOT=${CMAKE_SOURCE_DIR}/third_party/AudioUnitSDK-1.1.0
        -DSUPPLE_SANITIZE=${plugin_sanitize}
        -DFETCHCONTENT_FULLY_DISCONNECTED=ON
        -DFETCHCONTENT_SOURCE_DIR_CLAP=${tp}/clap
        -DFETCHCONTENT_SOURCE_DIR_CLAP-WRAPPER=${tp}/clap-wrapper
        -DFETCHCONTENT_SOURCE_DIR_DR_LIBS=${tp}/dr_libs/include
        -DFETCHCONTENT_SOURCE_DIR_PUGL=${tp}/pugl
        -DFETCHCONTENT_SOURCE_DIR_NANOVG=${tp}/nanovg
        -DFETCHCONTENT_SOURCE_DIR_NANOSVG=${tp}/nanosvg
      BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel 1 ${plugin_build_targets})
    set(staged "${CMAKE_BINARY_DIR}/plugins/clap")
    if(APPLE)
      set(_stage_notices "${plugin_build}/plugins/${artifact}.clap/Contents/Resources")
      set(_stage_clap ${CMAKE_COMMAND} -E copy_directory "${plugin_build}/plugins/${artifact}.clap" "${staged}/${artifact}.clap")
    else()
      set(_stage_notices "${plugin_build}/plugins")
      set(_stage_clap ${CMAKE_COMMAND} -E copy_if_different "${plugin_build}/plugins/${artifact}.clap" "${staged}/${artifact}.clap")
    endif()
    ExternalProject_Add_Step(plugin-${name} stage
      COMMAND ${CMAKE_COMMAND} -E make_directory "${staged}"
      COMMAND ${_stage_clap}
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_stage_notices}/$<IF:$<BOOL:${APPLE}>,LICENSE.txt,${artifact}-LICENSE.txt>" "${staged}/${artifact}-LICENSE.txt"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_stage_notices}/$<IF:$<BOOL:${APPLE}>,NOTICES.txt,${artifact}-NOTICES.txt>" "${staged}/${artifact}-NOTICES.txt"
      DEPENDEES build
      ALWAYS TRUE)
    if(SUPPLE_BUILD_TESTS)
      add_test(NAME plugin.${name}.ctest
        COMMAND ${CMAKE_CTEST_COMMAND} --test-dir "${plugin_build}" --output-on-failure)
    endif()
    set(${name}_clap "${staged}/${artifact}.clap" PARENT_SCOPE)
  endfunction()

  # clap-validator's runner (tools/validator/run.sh) is a bash script; on
  # Windows, Git's bash runs it.
  find_program(SUPPLE_BASH bash HINTS "$ENV{ProgramFiles}/Git/bin")

  # The player runs DSP scripts (ADR-0032): its plugins are the scripts in
  # SUPPLE_SCRIPT_LIBRARY's folders, so the checks every stock plugin has run
  # with the player's test scripts (Smoke; Tilt, which keeps state; Pluck,
  # an instrument).
  supple_add_template_product(player suite/player "Script Player")
  if(SUPPLE_BUILD_TESTS)
    set(player_scripts "${CMAKE_CURRENT_SOURCE_DIR}/suite/player/test")
    if(WIN32)
      set(player_env "SUPPLE_SCRIPT_LIBRARY=${player_scripts}/smoke\\;${player_scripts}/library")
    else()
      set(player_env "SUPPLE_SCRIPT_LIBRARY=${player_scripts}/smoke:${player_scripts}/library")
    endif()
    set(player_tests plugin.player.reset plugin.player.semantic.smoke plugin.player.semantic.tilt)
    add_test(NAME plugin.player.reset COMMAND clap-reset-check "${player_clap}")
    set_tests_properties(plugin.player.reset PROPERTIES LABELS "rt;plugin-reset" TIMEOUT 180)
    add_test(NAME plugin.player.semantic.smoke
      COMMAND semantic-conformance "${player_clap}" com.suppleaudio.script-player)
    add_test(NAME plugin.player.semantic.tilt COMMAND semantic-conformance "${player_clap}" com.example.tilt)
    add_test(NAME plugin.player.semantic.pluck COMMAND semantic-conformance "${player_clap}" com.example.pluck)
    list(APPEND player_tests plugin.player.semantic.pluck)
    if(UNIX AND NOT APPLE)
      add_test(NAME plugin.player.unload COMMAND clap-unload-probe "${player_clap}")
      set_tests_properties(plugin.player.unload PROPERTIES LABELS "rt;plugin-unload" TIMEOUT 30)
      list(APPEND player_tests plugin.player.unload)
    endif()
    if(NOT SUPPLE_SANITIZE AND SUPPLE_BASH)
      # The player has factory presets, on which 0.4.1's preset-discovery
      # tests deadlock (the template's validate.cmake skips them too).
      add_test(NAME plugin.player.validator
        COMMAND "${SUPPLE_BASH}" "${CMAKE_SOURCE_DIR}/tools/validator/run.sh"
                --exclude "^preset-discovery-(crawl|load)$" "${player_clap}")
      set_tests_properties(plugin.player.validator PROPERTIES LABELS slow SKIP_RETURN_CODE 77)
      list(APPEND player_tests plugin.player.validator)
    endif()
    set_tests_properties(${player_tests} PROPERTIES ENVIRONMENT "${player_env}")
    # Every example script as the player's library, through the reset check
    # (and so, in the rtsan preset, under RealtimeSanitizer).
    set(example_library "${CMAKE_CURRENT_BINARY_DIR}/script-examples")
    file(REMOVE_RECURSE "${example_library}")
    # (Not the sampler, which is silent without a sample: processor-tests
    # plays it a file.)
    file(GLOB example_scripts CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/suite/examples/*.cpp")
    list(FILTER example_scripts EXCLUDE REGEX "/sampler\\.cpp$")
    foreach(script IN LISTS example_scripts)
      get_filename_component(name "${script}" NAME_WE)
      configure_file("${script}" "${example_library}/${name}/${name}.cpp" COPYONLY)
    endforeach()
    add_test(NAME plugin.player.examples.reset COMMAND clap-reset-check "${player_clap}")
    set_tests_properties(plugin.player.examples.reset PROPERTIES LABELS "rt;plugin-reset" TIMEOUT 300
      ENVIRONMENT "SUPPLE_SCRIPT_LIBRARY=${example_library}")
    # Fresh source exports of every example plus the non-default library
    # tilt regression: each its own tests, semantic conformance and player audio parity.
    add_test(NAME plugin.export.source
      COMMAND ${CMAKE_COMMAND} -DSANDBOX=$<TARGET_FILE:${PRODUCT_SLUG}-sandbox>
              -DPLAYER=${player_clap} -DKIT=${CMAKE_BINARY_DIR}/plugins/script-export
              -DSCRIPT=${player_scripts}/library/tilt/tilt.cpp
              -DEXAMPLES=${CMAKE_CURRENT_SOURCE_DIR}/suite/examples
              -DGAIN_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/suite/examples/gain.cpp
              -DCONFORMANCE=$<TARGET_FILE:semantic-conformance>
              "-DPATCH={\"tilt\": 4.5, \"pivot\": 700, \"output\": -2}"
              -DWORK=${CMAKE_BINARY_DIR}/plugins/export-test -DTHIRD_PARTY=${CMAKE_SOURCE_DIR}/third_party
              -DCC=${CMAKE_C_COMPILER} -DCXX=${CMAKE_CXX_COMPILER} -DBUILD_TYPE=${CMAKE_BUILD_TYPE}
              -DTOOLCHAIN=${CMAKE_TOOLCHAIN_FILE}
              -DDEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
              -DARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
              -DCONFIGURE_JOBS=$<IF:$<BOOL:${APPLE}>,2,3>
              -P ${CMAKE_CURRENT_SOURCE_DIR}/suite/tests/source_export.cmake)
    # Nested builds use three jobs too; do not overlap them with other tests.
    set_tests_properties(plugin.export.source PROPERTIES LABELS slow TIMEOUT 3600 RUN_SERIAL TRUE)
  endif()

  # The stock plugins written as DSP scripts (plugins/README.md): each
  # folder is a source export's product folder holding its script, built
  # from the template like the player, with the checks every stock plugin
  # has. supple_add_stock_script(<folder> <artifact> <CLAP id>).
  function(supple_add_stock_script name artifact id)
    supple_add_template_product(${name} ${name} "${artifact}")
    if(SUPPLE_BUILD_TESTS)
      set(clap "${CMAKE_BINARY_DIR}/plugins/clap/${artifact}.clap")
      add_test(NAME plugin.${name}.reset COMMAND clap-reset-check "${clap}")
      # Again with its switches and choices moved too: a script whose default
      # state is a passthrough (an EQ with every band off) shows its state there.
      add_test(NAME plugin.${name}.reset-stepped COMMAND clap-reset-check "${clap}" ${id} --stepped)
      set_tests_properties(plugin.${name}.reset plugin.${name}.reset-stepped PROPERTIES
        LABELS "rt;plugin-reset" TIMEOUT 180)
      # Bit-identical output in any host blocks, with every parameter automated.
      add_test(NAME plugin.${name}.blocks COMMAND clap-block-check "${clap}" ${id})
      set_tests_properties(plugin.${name}.blocks PROPERTIES LABELS rt TIMEOUT 180)
      # Damaged saved state: refused with nothing changed, or accepted whole.
      add_test(NAME plugin.${name}.state COMMAND clap-state-check "${clap}" ${id})
      set_tests_properties(plugin.${name}.state PROPERTIES LABELS rt TIMEOUT 300)
      add_test(NAME plugin.${name}.semantic COMMAND semantic-conformance "${clap}" ${id})
      # Catalog.cpp, the folder's one generated file, still matches the script.
      add_test(NAME plugin.${name}.catalog
        COMMAND ${CMAKE_COMMAND} -DSANDBOX=$<TARGET_FILE:${PRODUCT_SLUG}-sandbox> -DNAME=${name}
                -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/${name}
                -DWORK=${CMAKE_BINARY_DIR}/plugins/stock-test/${name}-catalog
                -P ${CMAKE_CURRENT_SOURCE_DIR}/catalog.cmake)
      # The plugin's own measurements, where it has them.
      if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${name}/response.cmake")
        add_test(NAME plugin.${name}.response
          COMMAND ${CMAKE_COMMAND} -DSANDBOX=$<TARGET_FILE:${PRODUCT_SLUG}-sandbox> -DPLUGIN=${clap}
                  -DWORK=${CMAKE_BINARY_DIR}/plugins/stock-test/${name}-response
                  -P ${CMAKE_CURRENT_SOURCE_DIR}/${name}/response.cmake)
      endif()
      if(UNIX AND NOT APPLE)
        add_test(NAME plugin.${name}.unload COMMAND clap-unload-probe "${clap}")
        set_tests_properties(plugin.${name}.unload PROPERTIES LABELS "rt;plugin-unload" TIMEOUT 30)
      endif()
      if(NOT SUPPLE_SANITIZE AND SUPPLE_BASH)
        # Factory presets: clap-validator 0.4.1 deadlocks in preset discovery.
        add_test(NAME plugin.${name}.validator
          COMMAND "${SUPPLE_BASH}" "${CMAKE_SOURCE_DIR}/tools/validator/run.sh"
                  --exclude "^preset-discovery-(crawl|load)$" "${clap}")
        # Debug builds take minutes: the validator activates each plug-in
        # dozens of times, and a sample instrument resamples its kit on every
        # activation away from the kit's rate (Pollard: 5 of the 10 minutes
        # the presets allow, on a slow runner more).
        set_tests_properties(plugin.${name}.validator PROPERTIES LABELS slow SKIP_RETURN_CODE 77
                             TIMEOUT 1500)
      endif()
    endif()
  endfunction()
  supple_add_stock_script(yarrow Yarrow com.suppleaudio.yarrow)
  supple_add_stock_script(sundew Sundew com.suppleaudio.sundew)
  supple_add_stock_script(treeline Treeline com.suppleaudio.treeline)
  supple_add_stock_script(gorse Gorse com.suppleaudio.gorse)
  supple_add_stock_script(hickory Hickory com.suppleaudio.hickory)
  supple_add_stock_script(sorrel Sorrel com.suppleaudio.sorrel)
  supple_add_stock_script(teasel Teasel com.suppleaudio.teasel)
  supple_add_stock_script(bracken Bracken com.suppleaudio.bracken)
  supple_add_stock_script(ghyll Ghyll com.suppleaudio.ghyll)
  supple_add_stock_script(lichen Lichen com.suppleaudio.lichen)
  supple_add_stock_script(marram Marram com.suppleaudio.marram)
  supple_add_stock_script(withy Withy com.suppleaudio.withy)
  supple_add_stock_script(pollia Pollia com.suppleaudio.pollia)
  supple_add_stock_script(umber Umber com.suppleaudio.umber)
  supple_add_stock_script(sedge Sedge com.suppleaudio.sedge)
  supple_add_stock_script(gain Gain com.suppleaudio.gain)
  supple_add_stock_script(hornbeam Hornbeam com.suppleaudio.hornbeam)
  supple_add_stock_script(coppice Coppice com.suppleaudio.coppice)
  supple_add_stock_script(pollard Pollard com.suppleaudio.pollard)
  supple_add_stock_script(drumlin Drumlin com.suppleaudio.drumlin)
  if(SUPPLE_BUILD_TESTS)
    # Pollard plays its pads with Coppice's engine, kept in both scripts.
    add_test(NAME plugin.pollard.engine-copy
      COMMAND ${CMAKE_COMMAND} -DCOPPICE=${CMAKE_CURRENT_SOURCE_DIR}/coppice/script/coppice.cpp
              -DPOLLARD=${CMAKE_CURRENT_SOURCE_DIR}/pollard/script/pollard.cpp
              -P ${CMAKE_CURRENT_SOURCE_DIR}/pollard/engine-copy.cmake)
  endif()

  # A plugin taken out of the suite leaves its staged files behind, and the
  # app loads every plugin in plugins/clap: keep only what this build makes.
  get_property(staged_plugins GLOBAL PROPERTY SUPPLE_STAGED_PLUGINS)
  file(GLOB staged_files LIST_DIRECTORIES true "${CMAKE_BINARY_DIR}/plugins/clap/*")
  foreach(path IN LISTS staged_files)
    get_filename_component(file "${path}" NAME)
    if(file MATCHES "^(.+)(\\.clap|\\.vst3|\\.component|-LICENSE\\.txt|-NOTICES\\.txt)$"
       AND NOT CMAKE_MATCH_1 IN_LIST staged_plugins)
      message(STATUS "Removing ${file} from plugins/clap: no plugin here builds it any more")
      file(REMOVE_RECURSE "${path}")
    endif()
  endforeach()
endif()
