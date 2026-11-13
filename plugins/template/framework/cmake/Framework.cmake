# The framework build: every target, test and install rule, generated from
# product/product.cmake. Nothing in here names a product.

set(SUPPLE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(SUPPLE_FRAMEWORK ${SUPPLE_ROOT}/framework)
# product/ beside framework/, or the folder the build names (the root
# CMakeLists.txt): product.cmake's paths that start product/ are in it.
if (NOT SUPPLE_PRODUCT_DIR)
    set(SUPPLE_PRODUCT_DIR ${SUPPLE_ROOT}/product)
endif()
get_filename_component(SUPPLE_PRODUCT "${SUPPLE_PRODUCT_DIR}" ABSOLUTE BASE_DIR "${SUPPLE_ROOT}")
set(SUPPLE_GENERATED ${CMAKE_BINARY_DIR}/generated)
# VERSION.txt, never VERSION: framework/ is an include root, and on a
# case-insensitive filesystem (macOS, Windows) a file called VERSION is what
# the standard library's own #include <version> finds.
file(READ ${SUPPLE_FRAMEWORK}/VERSION.txt SUPPLE_FRAMEWORK_VERSION)
string(STRIP "${SUPPLE_FRAMEWORK_VERSION}" SUPPLE_FRAMEWORK_VERSION)
include(${SUPPLE_FRAMEWORK}/cmake/CheckIncludeRoots.cmake)
supple_check_include_roots(${SUPPLE_FRAMEWORK} ${SUPPLE_FRAMEWORK}/test ${SUPPLE_PRODUCT})
message(STATUS "${PRODUCT_NAME} ${PRODUCT_VERSION} on framework ${SUPPLE_FRAMEWORK_VERSION}")

# Bundle staging, AU preparation, install rules and tests all name
# plugins/<Name>.<ext>; a multi-config generator (Visual Studio, Xcode, Ninja
# Multi-Config) adds a per-configuration directory under that and, with
# clap-wrapper, shares some bundles between configurations. One build
# directory per configuration, which the presets already give.
get_property(supple_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if (supple_multi_config)
    message(FATAL_ERROR "${CMAKE_GENERATOR} is a multi-configuration generator; use Ninja or Makefiles "
                        "(on Windows, from a Visual Studio developer prompt: cmake --preset release -G Ninja)")
endif()

option(SUPPLE_BUILD_PLUGIN "Build the CLAP, VST3 and AUv2 plugins" ON)
# Off builds the CLAP (and the AU) alone, without the VST3 SDK: for a build
# that has the SDK's interfaces but not the rest clap-wrapper compiles.
option(SUPPLE_BUILD_VST3 "Build the VST3 beside the CLAP" ON)
option(SUPPLE_BUILD_GUI "Build the editor" ON)
option(SUPPLE_BUILD_TESTS "Build the tests, tools and probes" ON)
option(SUPPLE_WARNINGS_AS_ERRORS "Treat warnings in framework and product code as errors" OFF)
option(SUPPLE_FUZZ "Build libFuzzer targets (clang; the fuzz preset)" OFF)
if (SUPPLE_FUZZ AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SUPPLE_FUZZ needs clang (libFuzzer)")
endif()
set(SUPPLE_SANITIZE "" CACHE STRING "Instrument our code: empty, address (with undefined), thread, or realtime")
set_property(CACHE SUPPLE_SANITIZE PROPERTY STRINGS "" address thread realtime)
include(${SUPPLE_FRAMEWORK}/cmake/CheckSanitizer.cmake)
if (SUPPLE_SANITIZE STREQUAL "realtime" AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SUPPLE_SANITIZE=realtime needs clang 20 or later (RealtimeSanitizer); "
                        "this build uses ${CMAKE_CXX_COMPILER_ID}")
endif()
if (APPLE)
    # Apple deprecated OpenGL in 2018. It still works, but the direction is
    # not in doubt: macOS renders through Metal, GL everywhere else.
    option(SUPPLE_METAL "Render the editor through Metal" ON)
else()
    set(SUPPLE_METAL OFF)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

function(supple_sanitize target)
    if (SUPPLE_SANITIZE STREQUAL "")
        return()
    elseif (SUPPLE_SANITIZE STREQUAL "address")
        set(flags -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
        # Clang's function check reads the eight bytes before an indirect
        # call's target for a type signature. Code a product generates (a
        # JIT) can start a page whose page before is not mapped, where the
        # check crashes the call; GCC has no such check.
        if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            list(APPEND flags -fno-sanitize=function)
        endif()
    elseif (SUPPLE_SANITIZE STREQUAL "thread")
        set(flags -fsanitize=thread)
    elseif (SUPPLE_SANITIZE STREQUAL "realtime")
        # Every function marked SUPPLE_REALTIME (core/Realtime.h) becomes a
        # real-time context at run time: an allocation, lock, blocking system
        # call or I/O anywhere beneath it aborts with a stack trace.
        set(flags -fsanitize=realtime)
        target_compile_definitions(${target} PRIVATE SUPPLE_RTSAN=1)
        if (UNIX AND NOT APPLE)
            # On Linux the runtime is a static archive, linked into
            # executables only. A plugin module resolves it from the host
            # that loads it, so test executables export their symbols, and
            # modules may leave the runtime's undefined -- the VST3 wrapper
            # links with --no-undefined, which this must come after.
            get_target_property(type ${target} TYPE)
            if (type STREQUAL "EXECUTABLE")
                target_link_options(${target} PRIVATE -rdynamic)
            elseif (type STREQUAL "MODULE_LIBRARY" OR type STREQUAL "SHARED_LIBRARY")
                target_link_libraries(${target} PRIVATE "-Wl,-z,undefs")
            endif()
        endif()
    else()
        message(FATAL_ERROR "SUPPLE_SANITIZE must be empty, address, thread or realtime")
    endif()
    target_compile_options(${target} PRIVATE ${flags})
    target_link_options(${target} PRIVATE ${flags})
endfunction()

# The fuzz preset: our own code carries libFuzzer's coverage instrumentation,
# so the fuzzer can see which branches of the state reader an input reached.
function(supple_fuzz_coverage target)
    if (SUPPLE_FUZZ)
        target_compile_options(${target} PRIVATE -fsanitize=fuzzer-no-link)
    endif()
endfunction()

# Framework and product code: warnings on, symbols hidden. Hidden visibility
# is what lets every product use the same C++ names: nothing but the entry
# points leaves the binary, so two products in one host cannot bind to each
# other's code (on Linux the export maps below enforce the same thing).
function(supple_defaults target)
    set_target_properties(${target} PROPERTIES
        C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden
        OBJC_VISIBILITY_PRESET hidden OBJCXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    if (WIN32)
        # <windows.h> arrives through pugl's and GL's headers as well as our
        # own; without these it defines min and max as macros, which turns
        # every std::min and std::max after it into a syntax error.
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        # The CRT deprecates fopen, strcpy and friends in favour of its own
        # _s variants (C4996); the framework uses the portable ones.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    if (MSVC)
        # C4324: a struct padded to honour alignas -- the cache-line spacing
        # in Ring is that padding on purpose. C4127: a condition on a
        # compile-time constant (a product's kInstrument, kAnyRestart), which
        # is how the framework writes product-dependent code. C4702:
        # unreachable code, which is what the rest of a function is after an
        # `if constexpr (...) return;` for the products that take it.
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /wd4324 /wd4127 /wd4702)
        if (SUPPLE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic
            $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor -Woverloaded-virtual>)
        if (SUPPLE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
    supple_sanitize(${target})
    supple_fuzz_coverage(${target})
endfunction()

include(${SUPPLE_FRAMEWORK}/cmake/Dependencies.cmake)

# --- generated identity -----------------------------------------------------------------

function(supple_c_string out value)
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    set(${out} "${value}" PARENT_SCOPE)
endfunction()
supple_c_string(PRODUCT_NAME_C "${PRODUCT_NAME}")
supple_c_string(PRODUCT_DESCRIPTION_C "${PRODUCT_DESCRIPTION}")
supple_c_string(PRODUCT_VENDOR_C "${PRODUCT_VENDOR}")
supple_c_string(PRODUCT_URL_C "${PRODUCT_URL}")
include(${SUPPLE_FRAMEWORK}/cmake/Capabilities.cmake)
configure_file(${SUPPLE_FRAMEWORK}/plugin/ProductInfo.h.in ${SUPPLE_GENERATED}/ProductInfo.h @ONLY)

# A path product.cmake gives: product/... in the product's folder, anything
# else relative from the root.
function(supple_product_path path out)
    if (IS_ABSOLUTE "${path}")
        set(${out} ${path} PARENT_SCOPE)
    elseif (path MATCHES "^product/(.*)")
        set(${out} ${SUPPLE_PRODUCT}/${CMAKE_MATCH_1} PARENT_SCOPE)
    else()
        set(${out} ${SUPPLE_ROOT}/${path} PARENT_SCOPE)
    endif()
endfunction()
function(supple_sources out)
    set(result "")
    foreach(source ${ARGN})
        supple_product_path(${source} resolved)
        list(APPEND result ${resolved})
    endforeach()
    set(${out} ${result} PARENT_SCOPE)
endfunction()
supple_sources(product_dsp_sources ${PRODUCT_DSP_SOURCES})
supple_sources(product_editor_sources ${PRODUCT_EDITOR_SOURCES})
supple_sources(product_test_sources ${PRODUCT_TEST_SOURCES})
supple_sources(product_layer_sources ${PRODUCT_LAYER_SOURCES})
if (product_layer_sources AND NOT PRODUCT_LAYER)
    message(FATAL_ERROR "PRODUCT_LAYER_SOURCES needs PRODUCT_LAYER ON")
endif()

# --- the product ----------------------------------------------------------------------------

# The product's assets (PRODUCT_ASSETS), compiled in like the fonts, and the
# table core/Assets.h finds them by: file name, data, size.
set(asset_out ${SUPPLE_GENERATED}/assets)
set(asset_sources "")
set(asset_names "")
set(asset_externs "")
set(asset_rows "")
set(asset_index 0)
foreach(asset ${PRODUCT_ASSETS})
    supple_product_path(${asset} asset_file)
    if (NOT EXISTS ${asset_file})
        message(FATAL_ERROR "PRODUCT_ASSETS: ${asset} does not exist")
    endif()
    get_filename_component(asset_name ${asset} NAME)
    if (asset_name IN_LIST asset_names)
        message(FATAL_ERROR "PRODUCT_ASSETS: two assets are called ${asset_name}; they are found by file name")
    endif()
    list(APPEND asset_names ${asset_name})
    set(symbol supple_asset_${asset_index})
    add_custom_command(OUTPUT ${asset_out}/${symbol}.c
        COMMAND ${CMAKE_COMMAND} -DINPUT=${asset_file} -DOUTPUT=${asset_out}/${symbol}.c -DSYMBOL=${symbol}
                -P ${SUPPLE_FRAMEWORK}/cmake/Bin2c.cmake
        DEPENDS ${asset_file} ${SUPPLE_FRAMEWORK}/cmake/Bin2c.cmake
        COMMENT "Embedding ${asset_name}" VERBATIM)
    list(APPEND asset_sources ${asset_out}/${symbol}.c)
    supple_c_string(asset_name_c "${asset_name}")
    file(SIZE ${asset_file} asset_size)
    string(APPEND asset_externs "extern const unsigned char ${symbol}[];\n")
    string(APPEND asset_rows "    {\"${asset_name_c}\", ${symbol}, ${asset_size}},\n")
    math(EXPR asset_index "${asset_index} + 1")
endforeach()
# The size is read when configuring; changing an asset's size reconfigures.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PRODUCT_ASSETS})
if (asset_index EQUAL 0)
    set(asset_rows "    {0, 0, 0},\n")
endif()
set(asset_table "// Generated by framework/cmake/Framework.cmake from PRODUCT_ASSETS. Do not edit.
#include <stddef.h>

struct supple_asset {
    const char* name;
    const unsigned char* data;
    size_t size;
};
${asset_externs}
const struct supple_asset supple_assets[] = {
${asset_rows}};
const size_t supple_asset_count = ${asset_index};
")
file(CONFIGURE OUTPUT ${asset_out}/supple_assets.c CONTENT "${asset_table}" @ONLY)
list(APPEND asset_sources ${asset_out}/supple_assets.c)
add_library(product-assets STATIC ${asset_sources})
supple_vendored(product-assets)

# Everything SDK-free: parameters, the engine, the product's DSP. A product
# with tables makes its Definition from them (core/ProductDefinition.cpp); a
# run-time product makes its own, through its catalog (core/Catalog.h).
set(product_definition_sources ${SUPPLE_FRAMEWORK}/core/ProductDefinition.cpp)
if (PRODUCT_RUNTIME)
    set(product_definition_sources)
endif()
add_library(product-core STATIC ${SUPPLE_FRAMEWORK}/core/Param.cpp ${SUPPLE_FRAMEWORK}/core/PresetLibrary.cpp
    ${SUPPLE_FRAMEWORK}/core/Definition.cpp ${SUPPLE_FRAMEWORK}/core/Catalog.cpp ${product_definition_sources}
    ${SUPPLE_FRAMEWORK}/core/Files.cpp ${SUPPLE_FRAMEWORK}/dsp/AudioFile.cpp ${product_dsp_sources})
target_include_directories(product-core PUBLIC ${SUPPLE_FRAMEWORK} ${SUPPLE_PRODUCT} ${SUPPLE_GENERATED})
target_compile_definitions(product-core PUBLIC SUPPLE_PRODUCT_RUNTIME=$<BOOL:${PRODUCT_RUNTIME}>)
find_package(Threads REQUIRED)
target_link_libraries(product-core PUBLIC product-assets supple-drlibs Threads::Threads)
supple_defaults(product-core)

if (SUPPLE_BUILD_GUI)
    function(supple_gui_library name metal)
        set(sources
            ${SUPPLE_FRAMEWORK}/gui/EditorBase.cpp ${SUPPLE_FRAMEWORK}/gui/Fonts.cpp
            ${SUPPLE_FRAMEWORK}/gui/Gui.cpp ${SUPPLE_FRAMEWORK}/gui/Images.cpp
            ${SUPPLE_FRAMEWORK}/gui/NativeFiles.cpp ${SUPPLE_FRAMEWORK}/gui/Offscreen.cpp
            ${SUPPLE_FRAMEWORK}/gui/Ui.cpp ${SUPPLE_FRAMEWORK}/gui/Widgets.cpp ${SUPPLE_FRAMEWORK}/gui/Window.cpp
            ${product_editor_sources})
        # A product with no editor code gets the automatic one.
        if (NOT product_editor_sources)
            list(APPEND sources ${SUPPLE_FRAMEWORK}/gui/AutoEditor.cpp)
        endif()
        if (APPLE)
            list(APPEND sources ${SUPPLE_FRAMEWORK}/gui/NativeFilesMac.mm)
            set_source_files_properties(${SUPPLE_FRAMEWORK}/gui/NativeFilesMac.mm PROPERTIES COMPILE_OPTIONS "-fno-objc-arc")
        endif()
        if (metal)
            list(APPEND sources ${SUPPLE_FRAMEWORK}/gui/CanvasMetal.m)
            set_source_files_properties(${SUPPLE_FRAMEWORK}/gui/CanvasMetal.m PROPERTIES COMPILE_OPTIONS "-fno-objc-arc")
        else()
            list(APPEND sources ${SUPPLE_FRAMEWORK}/gui/CanvasGL.c)
        endif()
        add_library(${name} STATIC ${sources})
        # dlopen, for EGL (gui/Offscreen.cpp).
        target_link_libraries(${name} PUBLIC product-core supple-fonts supple-nanosvg ${CMAKE_DL_LIBS})
        if (WIN32)
            target_link_libraries(${name} PUBLIC ole32 shell32 uuid)
        elseif (APPLE)
            target_link_libraries(${name} PUBLIC "-framework Cocoa")
        endif()
        if (metal)
            target_link_libraries(${name} PUBLIC supple-nanovg-mtl supple-pugl)
            target_compile_definitions(${name} PUBLIC SUPPLE_METAL=1)
        else()
            target_link_libraries(${name} PUBLIC supple-nanovg $<IF:$<TARGET_EXISTS:supple-pugl-gl>,supple-pugl-gl,supple-pugl>)
            target_compile_definitions(${name} PUBLIC SUPPLE_METAL=0)
        endif()
        supple_defaults(${name})
    endfunction()
    supple_gui_library(product-gui ${SUPPLE_METAL})
    if (SUPPLE_METAL)
        # The preview tool reads back GL framebuffers; under Metal that would
        # be a different draw path from the one it exists to check.
        supple_gui_library(product-gui-gl OFF)
        set(SUPPLE_GL_GUI product-gui-gl)
    else()
        set(SUPPLE_GL_GUI product-gui)
    endif()
endif()

if (SUPPLE_BUILD_PLUGIN)
    add_library(product-impl STATIC ${SUPPLE_FRAMEWORK}/plugin/Plugin.cpp ${SUPPLE_FRAMEWORK}/plugin/PresetDiscovery.cpp
        ${product_layer_sources})
    target_link_libraries(product-impl PUBLIC product-core clap)
    # A product's layer (plugin/Layer.h): product::makeLayer, defined in
    # PRODUCT_LAYER_SOURCES or a library targets.cmake links in.
    target_compile_definitions(product-impl PUBLIC SUPPLE_PRODUCT_LAYER=$<BOOL:${PRODUCT_LAYER}>)
    # clapwrapper/vst3.h: the extension that tells the VST3 wrapper about
    # note expressions and MIDI channels.
    target_include_directories(product-impl PRIVATE ${clap-wrapper_SOURCE_DIR}/include)
    if (TARGET supple-mts)
        target_link_libraries(product-impl PRIVATE supple-mts)
        target_compile_definitions(product-impl PRIVATE SUPPLE_TUNING_MTS=1)
    endif()
    if (SUPPLE_BUILD_GUI)
        target_link_libraries(product-impl PUBLIC product-gui)
        target_compile_definitions(product-impl PUBLIC SUPPLE_WITH_GUI=1)
    else()
        target_compile_definitions(product-impl PUBLIC SUPPLE_WITH_GUI=0)
    endif()
    supple_defaults(product-impl)

    set(formats CLAP)
    if (SUPPLE_BUILD_VST3)
        list(APPEND formats VST3)
    endif()
    # An AU component is one plugin, its codes fixed in its bundle: a
    # run-time product, whose plugins are found when it runs, has none.
    if (APPLE AND NOT PRODUCT_RUNTIME)
        list(APPEND formats AUV2)
    endif()
    set(SUPPLE_ASSETS ${CMAKE_BINARY_DIR}/plugins)
    make_clapfirst_plugins(
        TARGET_NAME product-plugin
        IMPL_TARGET product-impl
        OUTPUT_NAME "${PRODUCT_NAME}"
        ENTRY_SOURCE "${SUPPLE_FRAMEWORK}/plugin/Entry.cpp"
        BUNDLE_IDENTIFIER "${PRODUCT_ID}"
        BUNDLE_VERSION "${PRODUCT_VERSION}"
        COPY_AFTER_BUILD FALSE
        PLUGIN_FORMATS ${formats}
        ASSET_OUTPUT_DIRECTORY ${SUPPLE_ASSETS}
        AUV2_MANUFACTURER_NAME "${PRODUCT_VENDOR}"
        AUV2_MANUFACTURER_CODE "${PRODUCT_AU_MANUFACTURER}"
        AUV2_SUBTYPE_CODE "${PRODUCT_AU_SUBTYPE}"
        AUV2_INSTRUMENT_TYPE "${au_type}")
    foreach(format clap vst3 auv2)
        if (TARGET product-plugin_${format})
            supple_sanitize(product-plugin_${format})
        endif()
    endforeach()

    # Linux modules export their entry points and nothing else. Anything more
    # is visible to every plugin in the process, and the GNU unique symbols
    # among it (fmt's locale facet id, the VST3 SDK's UpdateHandler) bind
    # across modules whatever RTLD_LOCAL says. -fvisibility cannot do this
    # alone: libstdc++ and pugl mark some of their own symbols default.
    if (UNIX AND NOT APPLE)
        foreach(format clap vst3)
            if (TARGET product-plugin_${format})
                set(map ${SUPPLE_FRAMEWORK}/cmake/exports-${format}.map)
                target_link_options(product-plugin_${format} PRIVATE "LINKER:--version-script=${map}")
                set_property(TARGET product-plugin_${format} APPEND PROPERTY LINK_DEPENDS ${map})
            endif()
        endforeach()
    endif()

    # The license and the third-party notices travel inside each bundle (or
    # beside the bare .clap/.vst3 files on Linux and Windows), because someone
    # handed only a binary never sees this repository. A target of its own,
    # always out of date, because a POST_BUILD on the plugins would only run
    # when they relink and editing the notices alone would ship the old copy.
    # The framework's notices, then any the product adds for its own
    # dependencies (product/NOTICES.txt). Composed at configure time; editing
    # either file reconfigures.
    set(notices ${SUPPLE_GENERATED}/NOTICES.txt)
    file(READ ${SUPPLE_FRAMEWORK}/NOTICES.txt notices_text)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SUPPLE_FRAMEWORK}/NOTICES.txt)
    # The glob, rechecked at every build, notices the file being created or
    # deleted; the configure dependency notices an edit.
    # PRODUCT_TUNING compiles in the MTS-ESP client, which brings its notice.
    if (TARGET supple-mts)
        file(READ ${mts-esp_SOURCE_DIR}/LICENSE mts_license)
        string(APPEND notices_text "\n\n--------------------------------------------------------------------------------\n"
               "MTS-ESP client library (https://github.com/ODDSound/MTS-ESP), 0BSD\n\n${mts_license}")
    endif()
    file(GLOB product_notices_file CONFIGURE_DEPENDS ${SUPPLE_PRODUCT}/NOTICES.txt)
    if (product_notices_file)
        file(READ ${SUPPLE_PRODUCT}/NOTICES.txt product_notices)
        string(APPEND notices_text "\n\n${product_notices}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SUPPLE_PRODUCT}/NOTICES.txt)
    endif()
    set(previous "")
    if (EXISTS ${notices})
        file(READ ${notices} previous)
    endif()
    if (NOT previous STREQUAL notices_text)  # unchanged text leaves the timestamp alone
        file(WRITE ${notices} "${notices_text}")
    endif()
    # The product's own LICENSE when its folder has one (a product kept
    # outside this tree), else the root's.
    set(license ${SUPPLE_ROOT}/LICENSE)
    if (EXISTS ${SUPPLE_PRODUCT}/LICENSE)
        set(license ${SUPPLE_PRODUCT}/LICENSE)
    endif()
    add_custom_target(product-notices ALL COMMENT "Staging the license and notices with the plugins")
    if (APPLE)
        foreach(spec "clap:${PRODUCT_NAME}.clap" "vst3:${PRODUCT_NAME}.vst3")
            string(REPLACE ":" ";" parts "${spec}")
            list(GET parts 0 format)
            list(GET parts 1 bundle)
            if (TARGET product-plugin_${format})
                add_dependencies(product-notices product-plugin_${format})
                add_custom_command(TARGET product-notices POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${SUPPLE_ASSETS}/${bundle}/Contents/Resources"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${notices}" "${SUPPLE_ASSETS}/${bundle}/Contents/Resources/NOTICES.txt"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${license}" "${SUPPLE_ASSETS}/${bundle}/Contents/Resources/LICENSE.txt"
                    VERBATIM)
            endif()
        endforeach()
    else()
        add_custom_command(TARGET product-notices POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SUPPLE_ASSETS}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${notices}" "${SUPPLE_ASSETS}/${PRODUCT_NAME}-NOTICES.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${license}" "${SUPPLE_ASSETS}/${PRODUCT_NAME}-LICENSE.txt"
            VERBATIM)
        # The Linux VST3 is a directory bundle, copied and installed whole:
        # its notices go inside it, not only beside it.
        if (NOT WIN32 AND TARGET product-plugin_vst3)
            set(resources "${SUPPLE_ASSETS}/${PRODUCT_NAME}.vst3/Contents/Resources")
            add_dependencies(product-notices product-plugin_vst3)
            add_custom_command(TARGET product-notices POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${resources}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${notices}" "${resources}/NOTICES.txt"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${license}" "${resources}/LICENSE.txt"
                VERBATIM)
        endif()
    endif()

    if (APPLE AND TARGET product-plugin_auv2)
        # CMake regenerates the bundle's generic Info.plist at configure time,
        # while clap-wrapper writes the real AudioComponents plist from its
        # build helper; an up-to-date AU binary does not rerun that copy, and
        # a component with CMake's plist is invisible to every host. Prepare
        # the bundle unconditionally, then sign it -- last, since adding a
        # file to a signed bundle invalidates the signature.
        set(component "${SUPPLE_ASSETS}/${PRODUCT_NAME}.component")
        add_custom_target(product-prepare-auv2 ALL
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/product-plugin_auv2-build-helper-output/auv2_Info.plist"
                "${component}/Contents/Info.plist"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${component}/Contents/Resources"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${notices}" "${component}/Contents/Resources/NOTICES.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${license}" "${component}/Contents/Resources/LICENSE.txt"
            COMMAND /usr/bin/codesign --force --deep --sign - --timestamp=none "${component}"
            DEPENDS product-plugin_auv2
            COMMENT "Preparing and ad-hoc signing ${PRODUCT_NAME}.component"
            VERBATIM)
    endif()

    # One target installs every format together, so a host never loads a
    # stale CLAP beside a fresh VST3. Ordinary builds never touch user plugin
    # folders.
    if (APPLE)
        set(install_specs
            "${PRODUCT_NAME}.clap|$ENV{HOME}/Library/Audio/Plug-Ins/CLAP|clap|dir"
            "${PRODUCT_NAME}.vst3|$ENV{HOME}/Library/Audio/Plug-Ins/VST3|vst3|dir"
            "${PRODUCT_NAME}.component|$ENV{HOME}/Library/Audio/Plug-Ins/Components|auv2|dir")
    elseif (WIN32)
        file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}" local_app_data)
        set(install_specs
            "${PRODUCT_NAME}.clap|${local_app_data}/Programs/Common/CLAP|clap|file"
            "${PRODUCT_NAME}.vst3|${local_app_data}/Programs/Common/VST3|vst3|file")
    else()
        set(install_specs
            "${PRODUCT_NAME}.clap|$ENV{HOME}/.clap|clap|file"
            "${PRODUCT_NAME}.vst3|$ENV{HOME}/.vst3|vst3|dir")
    endif()
    add_custom_target(product-install COMMENT "Installing every ${PRODUCT_NAME} format")
    add_dependencies(product-install product-notices)
    if (TARGET product-prepare-auv2)
        add_dependencies(product-install product-prepare-auv2)
    endif()
    foreach(spec ${install_specs})
        string(REPLACE "|" ";" parts "${spec}")
        list(GET parts 0 artifact)
        list(GET parts 1 destination)
        list(GET parts 2 format)
        list(GET parts 3 kind)
        if (NOT TARGET product-plugin_${format})
            continue()
        endif()
        add_dependencies(product-install product-plugin_${format})
        if (kind STREQUAL "dir")
            set(source "${SUPPLE_ASSETS}/${artifact}")
            set(copy copy_directory)
        else()
            set(source "$<TARGET_FILE:product-plugin_${format}>")
            set(copy copy)
        endif()
        add_custom_command(TARGET product-install POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${destination}/${artifact}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${destination}"
            COMMAND ${CMAKE_COMMAND} -E ${copy} "${source}" "${destination}/${artifact}"
            VERBATIM)
        if (NOT APPLE)
            add_custom_command(TARGET product-install POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${notices}" "${destination}/${PRODUCT_NAME}-NOTICES.txt"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${license}" "${destination}/${PRODUCT_NAME}-LICENSE.txt"
                VERBATIM)
        endif()
    endforeach()
    if (APPLE)
        add_custom_command(TARGET product-install POST_BUILD
            COMMAND /bin/sh -c "killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true" VERBATIM)
    endif()

    # Release packages (cmake/Package.cmake): an archive of every format with
    # the license, notices and install notes, and the platform's installer,
    # in build/<preset>/package. Signing is opt-in, by these or the same
    # names in the environment.
    set(SUPPLE_SIGN_IDENTITY "" CACHE STRING "macOS: Developer ID Application identity for the packaged bundles")
    set(SUPPLE_INSTALLER_IDENTITY "" CACHE STRING "macOS: Developer ID Installer identity for the .pkg")
    set(SUPPLE_NOTARY_PROFILE "" CACHE STRING "macOS: notarytool keychain profile; notarizes and staples")
    set(SUPPLE_SIGN_COMMAND "" CACHE STRING "Windows: a command that signs the file named after it (signtool ...)")
    if (WIN32)
        find_program(SUPPLE_ISCC NAMES iscc ISCC PATHS "$ENV{ProgramFiles\(x86\)}/Inno Setup 6" "$ENV{ProgramFiles}/Inno Setup 6")
    endif()
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" arch)
    if (APPLE AND CMAKE_OSX_ARCHITECTURES)
        list(LENGTH CMAKE_OSX_ARCHITECTURES n)
        if (n GREATER 1)
            set(arch universal)
        else()
            set(arch "${CMAKE_OSX_ARCHITECTURES}")
        endif()
    endif()
    if (arch MATCHES "^(amd64|x86_64|x64)$")
        set(arch x86_64)
    elseif (arch MATCHES "^(arm64|aarch64)$")
        set(arch arm64)
    endif()
    if (APPLE)
        set(SUPPLE_PLATFORM macos-${arch})
        set(package_formats -DCLAP=${SUPPLE_ASSETS}/${PRODUCT_NAME}.clap)
        if (TARGET product-plugin_vst3)
            list(APPEND package_formats -DVST3=${SUPPLE_ASSETS}/${PRODUCT_NAME}.vst3)
        endif()
        if (TARGET product-plugin_auv2)
            list(APPEND package_formats -DAU=${SUPPLE_ASSETS}/${PRODUCT_NAME}.component)
        endif()
    elseif (WIN32)
        set(SUPPLE_PLATFORM windows-${arch})
        set(package_formats -DCLAP=$<TARGET_FILE:product-plugin_clap>)
        if (TARGET product-plugin_vst3)
            list(APPEND package_formats -DVST3=$<TARGET_FILE:product-plugin_vst3>)
        endif()
    else()
        set(SUPPLE_PLATFORM linux-${arch})
        set(package_formats -DCLAP=$<TARGET_FILE:product-plugin_clap>)
        if (TARGET product-plugin_vst3)
            list(APPEND package_formats -DVST3=${SUPPLE_ASSETS}/${PRODUCT_NAME}.vst3)
        endif()
    endif()
    set(SUPPLE_PACKAGE_COMMAND ${CMAKE_COMMAND} "-DNAME=${PRODUCT_NAME}" "-DVERSION=${PRODUCT_VERSION}"
        "-DVENDOR=${PRODUCT_VENDOR}" "-DID=${PRODUCT_ID}" "-DPLATFORM=${SUPPLE_PLATFORM}"
        "-DLICENSE=${license}" "-DNOTICES=${notices}" ${package_formats}
        "-DSUPPLE_SIGN_IDENTITY=${SUPPLE_SIGN_IDENTITY}" "-DSUPPLE_INSTALLER_IDENTITY=${SUPPLE_INSTALLER_IDENTITY}"
        "-DSUPPLE_NOTARY_PROFILE=${SUPPLE_NOTARY_PROFILE}" "-DSUPPLE_SIGN_COMMAND=${SUPPLE_SIGN_COMMAND}")
    add_custom_target(product-package
        COMMAND ${SUPPLE_PACKAGE_COMMAND} "-DOUT=${CMAKE_BINARY_DIR}/package" -DINSTALLER=ON
                "-DISCC=${SUPPLE_ISCC}" -P ${SUPPLE_FRAMEWORK}/cmake/Package.cmake
        COMMENT "Packaging ${PRODUCT_NAME} ${PRODUCT_VERSION} for ${SUPPLE_PLATFORM}"
        VERBATIM USES_TERMINAL)
    add_dependencies(product-package product-notices)
    if (TARGET product-prepare-auv2)
        add_dependencies(product-package product-prepare-auv2)
    endif()

    # What tools/validate.cmake needs to know about this build.
    if (APPLE)
        set(validated_clap "${SUPPLE_ASSETS}/${PRODUCT_NAME}.clap")
    else()
        set(validated_clap "$<TARGET_FILE:product-plugin_clap>")
    endif()
    if (NOT TARGET product-plugin_vst3)
        set(validated_vst3 "")
    elseif (WIN32)
        set(validated_vst3 "$<TARGET_FILE:product-plugin_vst3>")
    else()
        set(validated_vst3 "${SUPPLE_ASSETS}/${PRODUCT_NAME}.vst3")
    endif()
    file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/supple-build.cmake CONTENT
"# Generated by framework/cmake/Framework.cmake for tools/validate.cmake.
set(BUILD_NAME \"${PRODUCT_NAME}\")
set(BUILD_CLAP \"${validated_clap}\")
set(BUILD_VST3 \"${validated_vst3}\")
set(BUILD_AU_TYPE \"${au_type}\")
set(BUILD_AU_SUBTYPE \"${PRODUCT_AU_SUBTYPE}\")
set(BUILD_AU_MANUFACTURER \"${PRODUCT_AU_MANUFACTURER}\")
set(BUILD_PRESETS \"${PRODUCT_PRESETS}\")
set(BUILD_SANITIZE \"${SUPPLE_SANITIZE}\")
")
endif()

if (SUPPLE_BUILD_TESTS)
    include(${SUPPLE_FRAMEWORK}/cmake/Tests.cmake)
endif()

# A product's own targets, when it has any: tools, measurement programs,
# extra test executables. Everything above is defined by now, so it can link
# product-core and call supple_defaults(); supple_test() and add_test() exist
# only when SUPPLE_BUILD_TESTS is on, so test targets go inside that check.
# It must not change any framework target.
if (EXISTS ${SUPPLE_PRODUCT}/targets.cmake)
    include(${SUPPLE_PRODUCT}/targets.cmake)
endif()
