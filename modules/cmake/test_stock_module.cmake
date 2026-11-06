# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Run: cmake -P modules/cmake/test_stock_module.cmake
get_filename_component(_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_test "${_root}/build/module-key-test")
file(MAKE_DIRECTORY "${_test}")
execute_process(COMMAND "${CMAKE_COMMAND}"
  "-DNAME=arrays" "-DSCRIPT=${_root}/plugins/suite/compiler/tests/module_arrays.h"
  "-DTEMPLATE=${CMAKE_CURRENT_LIST_DIR}/stock_module.cpp.in" "-DOUT=${_test}/arrays.cpp"
  -P "${CMAKE_CURRENT_LIST_DIR}/stock_module.cmake" RESULT_VARIABLE _result)
if(NOT _result EQUAL 0)
  message(FATAL_ERROR "stock wrapper generation failed")
endif()
file(READ "${_test}/arrays.cpp" _wrapper)
foreach(_pair "pitch,pitch" "enabled,enabled" "amount,amount" "signalIn,signal_in"
              "signalOut,signal_out" "gate,gate" "triggerOut,trigger_out")
  string(REPLACE "," ";" _parts "${_pair}")
  list(GET _parts 0 _member)
  list(GET _parts 1 _key)
  string(FIND "${_wrapper}" "{&S::${_member}, \"${_key}\"}" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "missing scalar/array member key: ${_member} -> ${_key}")
  endif()
endforeach()
message(STATUS "stock_module.cmake: scalar and declaration-array key lists agree")
