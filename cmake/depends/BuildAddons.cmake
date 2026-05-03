if(NOT DEFINED KODI_ADDONS_BINARY_DIR OR KODI_ADDONS_BINARY_DIR STREQUAL "")
  message(FATAL_ERROR "KODI_ADDONS_BINARY_DIR is required")
endif()
if(NOT DEFINED KODI_ADDONS_PACKAGE_TARGET OR KODI_ADDONS_PACKAGE_TARGET STREQUAL "")
  set(KODI_ADDONS_PACKAGE_TARGET "package-addons")
endif()
if(NOT DEFINED KODI_ADDONS_RESULTS_DIR OR KODI_ADDONS_RESULTS_DIR STREQUAL "")
  set(KODI_ADDONS_RESULTS_DIR "${KODI_ADDONS_BINARY_DIR}")
endif()

set(_success_file "${KODI_ADDONS_RESULTS_DIR}/.success")
set(_failure_file "${KODI_ADDONS_RESULTS_DIR}/.failure")
file(REMOVE "${_success_file}" "${_failure_file}")

if(NOT KODI_ADDONS_PACKAGE_ZIP)
  message(STATUS "Add-on packaging is disabled; skipping per-add-on package loop")
  file(WRITE "${_success_file}" "packaging-disabled\n")
  return()
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${KODI_ADDONS_BINARY_DIR}" --target supported_addons
  RESULT_VARIABLE _supported_rc
  OUTPUT_VARIABLE _supported_out
  ERROR_VARIABLE _supported_err
)

if(NOT _supported_rc EQUAL 0)
  message(FATAL_ERROR
    "Failed to read supported_addons target.\n${_supported_out}\n${_supported_err}")
endif()

set(_addons "")
string(REGEX MATCH "ALL_ADDONS_BUILDING:[ ]*([^\n\r]+)" _matched_line "${_supported_out}")
if(_matched_line)
  set(_addons_raw "${CMAKE_MATCH_1}")
  string(STRIP "${_addons_raw}" _addons_raw)
  if(NOT _addons_raw STREQUAL "")
    separate_arguments(_addons UNIX_COMMAND "${_addons_raw}")
  endif()
endif()

if(NOT _addons)
  message(STATUS "No supported add-ons returned. Falling back to ${KODI_ADDONS_PACKAGE_TARGET}.")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${KODI_ADDONS_BINARY_DIR}" --target "${KODI_ADDONS_PACKAGE_TARGET}"
    RESULT_VARIABLE _package_all_rc
  )
  if(NOT _package_all_rc EQUAL 0)
    message(FATAL_ERROR "Failed to build ${KODI_ADDONS_PACKAGE_TARGET}")
  endif()
  file(WRITE "${_success_file}" "${KODI_ADDONS_PACKAGE_TARGET}\n")
  return()
endif()

foreach(_addon IN LISTS _addons)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${KODI_ADDONS_BINARY_DIR}" --target "package-${_addon}"
    RESULT_VARIABLE _package_rc
    OUTPUT_VARIABLE _package_out
    ERROR_VARIABLE _package_err
  )
  if(_package_rc EQUAL 0)
    file(APPEND "${_success_file}" "${_addon}\n")
  else()
    message(STATUS "Add-on packaging failed for ${_addon}")
    file(APPEND "${_failure_file}" "${_addon}\n")
    message(STATUS "${_package_out}\n${_package_err}")
  endif()
endforeach()

if(EXISTS "${_failure_file}")
  file(READ "${_failure_file}" _failures)
  string(STRIP "${_failures}" _failures)
  if(NOT _failures STREQUAL "")
    message(FATAL_ERROR "One or more add-ons failed to package: ${_failures}")
  endif()
endif()

if(NOT EXISTS "${_success_file}")
  file(WRITE "${_success_file}" "no-addons-packaged\n")
endif()
