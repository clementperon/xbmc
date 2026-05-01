include_guard(GLOBAL)

if(NOT WIN32)
  find_program(KODI_DEPENDS_MAKE_EXECUTABLE NAMES gmake make REQUIRED)
endif()

function(kodi_depends_pair_dependencies pairs package_name out_var)
  set(_deps "")
  set(_active_pkg "")
  foreach(_pair IN LISTS pairs)
    if(_pair MATCHES "^([^:]+):(.*)$")
      set(_active_pkg "${CMAKE_MATCH_1}")
      set(_dep_string "${CMAKE_MATCH_2}")
      if(_active_pkg STREQUAL "${package_name}" AND _dep_string)
        string(REPLACE ";" " " _dep_space_sep "${_dep_string}")
        separate_arguments(_dep_list UNIX_COMMAND "${_dep_space_sep}")
        list(APPEND _deps ${_dep_list})
      endif()
      continue()
    endif()

    # Entries without ":" can appear because dependency pairs with semicolons
    # are split by CMake list parsing. Treat them as continuation deps for the
    # last package entry.
    if(_active_pkg STREQUAL "${package_name}" AND NOT _pair STREQUAL "")
      list(APPEND _deps "${_pair}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _deps)
  set(${out_var} "${_deps}" PARENT_SCOPE)
endfunction()

function(kodi_depends_add_make_target)
  set(options)
  set(oneValueArgs NAME PACKAGE PACKAGE_TYPE DEPENDS_TARGET)
  set(multiValueArgs EXTRA_DEPENDS)
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT ARG_NAME OR NOT ARG_PACKAGE OR NOT ARG_PACKAGE_TYPE)
    message(FATAL_ERROR "kodi_depends_add_make_target requires NAME, PACKAGE and PACKAGE_TYPE.")
  endif()

  if(NOT ARG_DEPENDS_TARGET)
    set(ARG_DEPENDS_TARGET kodi-depends-configure)
  endif()

  set(_package_rel_dir "${ARG_PACKAGE_TYPE}/${ARG_PACKAGE}")
  set(_package_abs_dir "${KODI_DEPENDS_SOURCE_DIR}/${_package_rel_dir}")

  if(WIN32)
    file(TO_CMAKE_PATH "${_package_abs_dir}" _package_abs_dir_msys)
    kodi_depends_get_shell_command(_build_cmd "make -C \"${_package_abs_dir_msys}\"")
    set(_target_command ${_build_cmd})
  else()
    set(_target_command ${KODI_DEPENDS_MAKE_EXECUTABLE} -C "${_package_abs_dir}")
  endif()

  add_custom_target(${ARG_NAME}
    COMMAND ${_target_command}
    WORKING_DIRECTORY "${KODI_DEPENDS_SOURCE_DIR}"
    USES_TERMINAL
    COMMENT "Building depends package ${_package_rel_dir}")

  add_dependencies(${ARG_NAME} ${ARG_DEPENDS_TARGET})
  if(ARG_EXTRA_DEPENDS)
    add_dependencies(${ARG_NAME} ${ARG_EXTRA_DEPENDS})
  endif()
endfunction()
