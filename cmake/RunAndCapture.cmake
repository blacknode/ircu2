#
# Run a program and capture its standard output into a file.
#
# CMake's add_custom_command has no portable shell redirection, so build
# steps that generate a source file on stdout go through this script:
#
#   cmake -DIRCU_COMMAND=<exe> -DIRCU_OUTPUT=<file> -P RunAndCapture.cmake
#

cmake_minimum_required(VERSION 3.16)

if(NOT IRCU_COMMAND OR NOT IRCU_OUTPUT)
  message(FATAL_ERROR "IRCU_COMMAND and IRCU_OUTPUT must both be set")
endif()

execute_process(
  COMMAND "${IRCU_COMMAND}"
  OUTPUT_FILE "${IRCU_OUTPUT}"
  RESULT_VARIABLE _result
  ERROR_VARIABLE _stderr)

if(NOT _result EQUAL 0)
  file(REMOVE "${IRCU_OUTPUT}")
  message(FATAL_ERROR
    "${IRCU_COMMAND} failed with status ${_result}: ${_stderr}")
endif()
