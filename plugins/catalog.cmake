# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# A stock script's Catalog.cpp is the one generated file in its folder: the
# patch key of each Param member, which a source export works out from the
# script (C++ cannot see member names). This checks it against a fresh
# export, or with -DUPDATE=ON writes the fresh one in its place.
# Required: SANDBOX (steppe-sandbox), NAME (the script), DIR (its folder), WORK.
foreach(var SANDBOX NAME DIR WORK)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "catalog: -D${var}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(COPY "${DIR}/script/${NAME}.cpp" DESTINATION "${WORK}/workdir/scripts/${NAME}")
# A private home keeps the user's plugins and scripts out of it.
set(home "${WORK}/home")
file(MAKE_DIRECTORY "${home}")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "HOME=${home}" "USERPROFILE=${home}" "APPDATA=${home}/config"
            "LOCALAPPDATA=${home}/local" "XDG_DATA_HOME=${home}/data" "${SANDBOX}" exec
            "{\"command\":\"dsp.export\",\"args\":{\"script\":\"${NAME}\",\"format\":\"source\",\"folder\":\"${WORK}/export\"}}"
            --workdir "${WORK}/workdir"
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0 OR NOT EXISTS "${WORK}/export/product/Catalog.cpp")
    message(FATAL_ERROR "exporting ${NAME} failed (${result}):\n${out}\n${err}")
endif()

# The plugin's CLAP features come from the script's Info; product.cmake keeps
# the same list for the template.
function(product_features file out)
    file(STRINGS "${file}" line REGEX "^set\\(PRODUCT_FEATURES +\"[^\"]*\"\\)")
    string(REGEX REPLACE "^set\\(PRODUCT_FEATURES +\"([^\"]*)\"\\).*" "\\1" value "${line}")
    set(${out} "${value}" PARENT_SCOPE)
endfunction()
product_features("${WORK}/export/product/product.cmake" declared)
product_features("${DIR}/product.cmake" kept_features)
if(NOT declared STREQUAL kept_features)
    message(FATAL_ERROR "${DIR}/product.cmake has PRODUCT_FEATURES \"${kept_features}\", but "
                        "script/${NAME}.cpp's Info declares .features {\"${declared}\"}: "
                        "make product.cmake match the script")
endif()

# Its extra outputs likewise, from the script's Info.outputs; none when
# product.cmake does not set them.
function(product_outputs file out)
    file(STRINGS "${file}" line REGEX "^set\\(PRODUCT_AUX_OUTPUTS .*\\)")
    string(REGEX REPLACE "^set\\(PRODUCT_AUX_OUTPUTS +(.*)\\)" "\\1" value "${line}")
    if(value STREQUAL "\"\"")
        set(value "")
    endif()
    set(${out} "${value}" PARENT_SCOPE)
endfunction()
product_outputs("${WORK}/export/product/product.cmake" declared)
product_outputs("${DIR}/product.cmake" kept_outputs)
if(NOT declared STREQUAL kept_outputs)
    message(FATAL_ERROR "${DIR}/product.cmake has PRODUCT_AUX_OUTPUTS ${kept_outputs}, but "
                        "script/${NAME}.cpp's Info declares .outputs ${declared}: "
                        "make product.cmake match the script")
endif()

file(READ "${WORK}/export/product/Catalog.cpp" fresh)
file(READ "${DIR}/Catalog.cpp" kept)
if(fresh STREQUAL kept)
    message(STATUS "${NAME}: Catalog.cpp matches the script")
elseif(UPDATE)
    file(WRITE "${DIR}/Catalog.cpp" "${fresh}")
    message(STATUS "${NAME}: Catalog.cpp updated")
else()
    message(FATAL_ERROR "${DIR}/Catalog.cpp no longer matches script/${NAME}.cpp (a Param was added, "
                        "removed or renamed). Regenerate it: cmake -DUPDATE=ON -DSANDBOX=<steppe-sandbox> "
                        "-DNAME=${NAME} -DDIR=${DIR} -DWORK=<scratch folder> -P ${CMAKE_CURRENT_LIST_FILE}")
endif()
