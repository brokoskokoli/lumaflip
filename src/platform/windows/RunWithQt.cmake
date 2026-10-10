# SPDX-License-Identifier: GPL-3.0-or-later
# Runs a freshly built Qt tool during the build (cmake -P): Qt's DLLs are not on PATH there.
#   -DLF_QT_BIN=<Qt bin dir> -DLF_TOOL=<exe> -DLF_ARGS=<arg1|arg2|...>
set(ENV{PATH} "${LF_QT_BIN};$ENV{PATH}")
string(REPLACE "|" ";" _args "${LF_ARGS}")
execute_process(COMMAND "${LF_TOOL}" ${_args} RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "${LF_TOOL} failed (${_rc})")
endif()
