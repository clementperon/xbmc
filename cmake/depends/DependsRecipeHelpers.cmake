include_guard(GLOBAL)

function(kodi_depends_pair_dependencies pairs package_name out_var)
  set(_deps "")
  foreach(_pair IN LISTS pairs)
    if(NOT _pair MATCHES "^([^:]+):(.*)$")
      continue()
    endif()
    set(_pkg "${CMAKE_MATCH_1}")
    set(_dep_string "${CMAKE_MATCH_2}")
    if(_pkg STREQUAL "${package_name}" AND _dep_string)
      string(REPLACE ";" " " _dep_space_sep "${_dep_string}")
      separate_arguments(_dep_list UNIX_COMMAND "${_dep_space_sep}")
      set(_deps ${_dep_list})
      break()
    endif()
  endforeach()
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

  add_custom_target(${ARG_NAME}
    COMMAND ${CMAKE_MAKE_PROGRAM} -C "${_package_abs_dir}"
    WORKING_DIRECTORY "${KODI_DEPENDS_SOURCE_DIR}"
    USES_TERMINAL
    COMMENT "Building depends package ${_package_rel_dir}")

  add_dependencies(${ARG_NAME} ${ARG_DEPENDS_TARGET})
  if(ARG_EXTRA_DEPENDS)
    add_dependencies(${ARG_NAME} ${ARG_EXTRA_DEPENDS})
  endif()
endfunction()
