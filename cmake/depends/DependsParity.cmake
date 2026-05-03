include_guard(GLOBAL)

function(_kodi_depends_normalize_list out_var)
  set(_items ${ARGN})
  if(_items)
    list(REMOVE_DUPLICATES _items)
    list(SORT _items)
  endif()
  set(${out_var} "${_items}" PARENT_SCOPE)
endfunction()

function(_kodi_depends_parse_make_database_var out_var make_output variable_name)
  string(REGEX MATCHALL "(^|\n)${variable_name}[ \t]*:?=[ \t]*[^\n\r]*" _matches "${make_output}")
  if(NOT _matches)
    message(FATAL_ERROR "Could not find ${variable_name} in legacy make database output")
  endif()

  list(GET _matches -1 _line)
  string(REGEX REPLACE "^\n" "" _line "${_line}")
  string(REGEX REPLACE "^${variable_name}[ \t]*:?=[ \t]*" "" _value "${_line}")
  string(STRIP "${_value}" _value)

  if(_value)
    separate_arguments(_items UNIX_COMMAND "${_value}")
  else()
    set(_items "")
  endif()

  set(${out_var} "${_items}" PARENT_SCOPE)
endfunction()

function(_kodi_depends_read_legacy_packages out_var make_dir variable_name)
  execute_process(
    COMMAND ${KODI_DEPENDS_MAKE_EXECUTABLE} -C "${make_dir}" -pn __kodi_depends_noop_target__
    WORKING_DIRECTORY "${KODI_DEPENDS_SOURCE_DIR}"
    RESULT_VARIABLE _make_rc
    OUTPUT_VARIABLE _make_out
    ERROR_VARIABLE _make_err)

  # The fake target intentionally returns non-zero. We only need GNU make's
  # expanded database, which is emitted before the missing-target diagnostic.
  if(NOT _make_out)
    message(FATAL_ERROR
      "Could not read legacy make database in ${make_dir}.\n${_make_err}")
  endif()

  _kodi_depends_parse_make_database_var(_packages "${_make_out}" "${variable_name}")
  set(${out_var} "${_packages}" PARENT_SCOPE)
endfunction()

function(_kodi_depends_compare_package_lists label legacy_packages cmake_packages)
  _kodi_depends_normalize_list(_legacy_sorted ${legacy_packages})
  _kodi_depends_normalize_list(_cmake_sorted ${cmake_packages})

  set(_missing_from_cmake ${_legacy_sorted})
  if(_missing_from_cmake)
    list(REMOVE_ITEM _missing_from_cmake ${_cmake_sorted})
  endif()

  set(_extra_in_cmake ${_cmake_sorted})
  if(_extra_in_cmake)
    list(REMOVE_ITEM _extra_in_cmake ${_legacy_sorted})
  endif()

  if(_missing_from_cmake OR _extra_in_cmake)
    string(REPLACE ";" ", " _missing_msg "${_missing_from_cmake}")
    string(REPLACE ";" ", " _extra_msg "${_extra_in_cmake}")
    message(FATAL_ERROR
      "CMake depends ${label} package inventory differs from tools/depends.\n"
      "Missing from CMake: ${_missing_msg}\n"
      "Extra in CMake: ${_extra_msg}")
  endif()

  list(LENGTH _cmake_sorted _count)
  message(STATUS "Verified ${label} depends package parity with tools/depends (${_count} packages)")
endfunction()

function(kodi_depends_verify_legacy_graph native_packages target_packages)
  if(WIN32)
    message(STATUS "Skipping legacy make graph parity check on Windows/MSYS")
    return()
  endif()

  if(NOT KODI_DEPENDS_MAKE_EXECUTABLE)
    message(FATAL_ERROR "KODI_DEPENDS_MAKE_EXECUTABLE is required for graph parity checks")
  endif()

  _kodi_depends_read_legacy_packages(_legacy_native
                                     "${KODI_DEPENDS_SOURCE_DIR}/native"
                                     "NATIVE")
  _kodi_depends_read_legacy_packages(_legacy_target
                                     "${KODI_DEPENDS_SOURCE_DIR}/target"
                                     "DEPENDS")

  _kodi_depends_compare_package_lists("native" "${_legacy_native}" "${native_packages}")
  _kodi_depends_compare_package_lists("target" "${_legacy_target}" "${target_packages}")
endfunction()
