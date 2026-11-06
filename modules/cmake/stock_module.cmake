# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Writes the C++ wrapper of one stock module (run at build time, so the
# wrapper and the source it embeds follow every edit of the script):
#
#   cmake -DNAME=<name> -DSCRIPT=<stock/name.cpp> -DTEMPLATE=<stock_module.cpp.in>
#         -DOUT=<wrapper.cpp> -P stock_module.cmake
#
# The module's struct is the one line 'struct <Name> : Module'. Its keys are
# what the script compiler gives: each Param, Input and Output member's name
# in snake_case, unless the member's withKey() says otherwise. C++ cannot see
# member names, so they are read from the declarations here and handed to
# NativeModuleCode (modules/tests/test_jit.cpp checks they agree).

file(STRINGS ${SCRIPT} _decl REGEX "^struct [A-Za-z_][A-Za-z0-9_]* : Module")
list(LENGTH _decl _count)
if(NOT _count EQUAL 1)
  message(FATAL_ERROR "modules/stock/${NAME}.cpp: expected one line 'struct <Name> : Module "
                      "{' at the start of a line, found ${_count}")
endif()
string(REGEX REPLACE "^struct ([A-Za-z_][A-Za-z0-9_]*) : Module.*$" "\\1" STRUCT "${_decl}")

# lowCut -> low_cut, HTTPServer -> http_server, osc2Level -> osc2_level: the
# compiler's rule (plugins/suite/compiler/src/sema.cpp, snakeCase).
function(_member_key name out)
  string(REGEX REPLACE "([A-Z])([A-Z][a-z])" "\\1_\\2" k "${name}")
  string(REGEX REPLACE "([a-z0-9])([A-Z])" "\\1_\\2" k "${k}")
  string(TOLOWER "${k}" k)
  set(${out} "${k}" PARENT_SCOPE)
endfunction()

set(_member "^[ \t]*(Param|Input|Output)[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*(=|{|;).*$")
file(STRINGS ${SCRIPT} _members REGEX "${_member}")
set(KEYS "")
set(PORTKEYS "")
foreach(_line IN LISTS _members)
  string(REGEX REPLACE "${_member}" "\\1" _type "${_line}")
  string(REGEX REPLACE "${_member}" "\\2" _name "${_line}")
  _member_key(${_name} _key)
  if(_type STREQUAL "Param")
    string(APPEND KEYS "{&S::${_name}, \"${_key}\"}, ")
  else()
    string(APPEND PORTKEYS "{&S::${_name}, \"${_key}\"}, ")
  endif()
endforeach()

file(READ ${SCRIPT} _hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," HEX "${_hex}")
set(SOURCE ${SCRIPT})
configure_file(${TEMPLATE} ${OUT} @ONLY)
# The wrapper includes the script, so it rebuilds whenever the script changed.
file(TOUCH_NOCREATE ${OUT})
