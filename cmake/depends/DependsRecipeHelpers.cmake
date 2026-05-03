include_guard(GLOBAL)

include(ExternalProject)

if(NOT WIN32)
  find_program(KODI_DEPENDS_MAKE_EXECUTABLE NAMES gmake make REQUIRED)
endif()

function(kodi_depends_get_package_dir out_var package_type package_name)
  set(_package_dir "${KODI_DEPENDS_SOURCE_DIR}/${package_type}/${package_name}")
  if(NOT IS_DIRECTORY "${_package_dir}")
    message(FATAL_ERROR "Missing depends package directory: ${_package_dir}")
  endif()

  set(${out_var} "${_package_dir}" PARENT_SCOPE)
endfunction()

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
  kodi_depends_get_package_dir(_package_abs_dir "${ARG_PACKAGE_TYPE}" "${ARG_PACKAGE}")

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

function(kodi_depends_add_external_recipe)
  set(options)
  set(oneValueArgs NAME PACKAGE PACKAGE_TYPE SOURCE_DIR BINARY_DIR INSTALL_DIR CONFIGURE_COMMAND BUILD_COMMAND INSTALL_COMMAND)
  set(multiValueArgs EXTRA_DEPENDS CMAKE_ARGS PATCH_COMMAND BUILD_BYPRODUCTS)
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT ARG_NAME OR NOT ARG_PACKAGE OR NOT ARG_PACKAGE_TYPE)
    message(FATAL_ERROR "kodi_depends_add_external_recipe requires NAME, PACKAGE and PACKAGE_TYPE.")
  endif()

  kodi_depends_get_package_dir(_package_abs_dir "${ARG_PACKAGE_TYPE}" "${ARG_PACKAGE}")

  if(NOT ARG_SOURCE_DIR)
    set(ARG_SOURCE_DIR "${_package_abs_dir}")
  endif()
  if(NOT ARG_BINARY_DIR)
    set(ARG_BINARY_DIR "${CMAKE_BINARY_DIR}/depends-recipes/${ARG_PACKAGE_TYPE}/${ARG_PACKAGE}")
  endif()
  if(NOT ARG_INSTALL_DIR)
    if(ARG_PACKAGE_TYPE STREQUAL "native")
      set(ARG_INSTALL_DIR "${KODI_DEPENDS_NATIVEPREFIX_RESOLVED}")
    else()
      set(ARG_INSTALL_DIR "${KODI_DEPENDS_PREFIX_RESOLVED}")
    endif()
  endif()

  set(_external_args
    SOURCE_DIR "${ARG_SOURCE_DIR}"
    BINARY_DIR "${ARG_BINARY_DIR}"
    INSTALL_DIR "${ARG_INSTALL_DIR}"
  )

  if(ARG_CONFIGURE_COMMAND)
    list(APPEND _external_args CONFIGURE_COMMAND ${ARG_CONFIGURE_COMMAND})
  endif()
  if(ARG_BUILD_COMMAND)
    list(APPEND _external_args BUILD_COMMAND ${ARG_BUILD_COMMAND})
  endif()
  if(ARG_INSTALL_COMMAND)
    list(APPEND _external_args INSTALL_COMMAND ${ARG_INSTALL_COMMAND})
  endif()
  if(ARG_PATCH_COMMAND)
    list(APPEND _external_args PATCH_COMMAND ${ARG_PATCH_COMMAND})
  endif()
  if(ARG_CMAKE_ARGS)
    list(APPEND _external_args CMAKE_ARGS ${ARG_CMAKE_ARGS})
  endif()
  if(ARG_BUILD_BYPRODUCTS)
    list(APPEND _external_args BUILD_BYPRODUCTS ${ARG_BUILD_BYPRODUCTS})
  endif()

  ExternalProject_Add(${ARG_NAME} ${_external_args})

  if(ARG_EXTRA_DEPENDS)
    add_dependencies(${ARG_NAME} ${ARG_EXTRA_DEPENDS})
  endif()
endfunction()

function(kodi_dep_cmake)
  set(options)
  set(oneValueArgs NAME PACKAGE PACKAGE_TYPE SOURCE_DIR BINARY_DIR INSTALL_DIR)
  set(multiValueArgs EXTRA_DEPENDS CMAKE_ARGS PATCH_COMMAND BUILD_BYPRODUCTS)
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT ARG_NAME)
    set(ARG_NAME "kodi-depends-${ARG_PACKAGE_TYPE}-${ARG_PACKAGE}")
  endif()

  kodi_depends_add_external_recipe(
    NAME "${ARG_NAME}"
    PACKAGE "${ARG_PACKAGE}"
    PACKAGE_TYPE "${ARG_PACKAGE_TYPE}"
    SOURCE_DIR "${ARG_SOURCE_DIR}"
    BINARY_DIR "${ARG_BINARY_DIR}"
    INSTALL_DIR "${ARG_INSTALL_DIR}"
    CMAKE_ARGS ${ARG_CMAKE_ARGS}
    PATCH_COMMAND ${ARG_PATCH_COMMAND}
    BUILD_BYPRODUCTS ${ARG_BUILD_BYPRODUCTS}
    EXTRA_DEPENDS ${ARG_EXTRA_DEPENDS})
endfunction()

function(kodi_dep_autotools)
  set(options)
  set(oneValueArgs NAME PACKAGE PACKAGE_TYPE SOURCE_DIR BINARY_DIR INSTALL_DIR CONFIGURE_COMMAND)
  set(multiValueArgs EXTRA_DEPENDS PATCH_COMMAND BUILD_BYPRODUCTS)
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT ARG_NAME)
    set(ARG_NAME "kodi-depends-${ARG_PACKAGE_TYPE}-${ARG_PACKAGE}")
  endif()
  if(NOT ARG_CONFIGURE_COMMAND)
    set(ARG_CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR>)
  endif()

  kodi_depends_add_external_recipe(
    NAME "${ARG_NAME}"
    PACKAGE "${ARG_PACKAGE}"
    PACKAGE_TYPE "${ARG_PACKAGE_TYPE}"
    SOURCE_DIR "${ARG_SOURCE_DIR}"
    BINARY_DIR "${ARG_BINARY_DIR}"
    INSTALL_DIR "${ARG_INSTALL_DIR}"
    CONFIGURE_COMMAND ${ARG_CONFIGURE_COMMAND}
    BUILD_COMMAND ${KODI_DEPENDS_MAKE_EXECUTABLE}
    INSTALL_COMMAND ${KODI_DEPENDS_MAKE_EXECUTABLE} install
    PATCH_COMMAND ${ARG_PATCH_COMMAND}
    BUILD_BYPRODUCTS ${ARG_BUILD_BYPRODUCTS}
    EXTRA_DEPENDS ${ARG_EXTRA_DEPENDS})
endfunction()

function(kodi_dep_meson)
  set(options)
  set(oneValueArgs NAME PACKAGE PACKAGE_TYPE SOURCE_DIR BINARY_DIR INSTALL_DIR)
  set(multiValueArgs EXTRA_DEPENDS MESON_ARGS PATCH_COMMAND BUILD_BYPRODUCTS)
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT ARG_NAME)
    set(ARG_NAME "kodi-depends-${ARG_PACKAGE_TYPE}-${ARG_PACKAGE}")
  endif()

  kodi_depends_add_external_recipe(
    NAME "${ARG_NAME}"
    PACKAGE "${ARG_PACKAGE}"
    PACKAGE_TYPE "${ARG_PACKAGE_TYPE}"
    SOURCE_DIR "${ARG_SOURCE_DIR}"
    BINARY_DIR "${ARG_BINARY_DIR}"
    INSTALL_DIR "${ARG_INSTALL_DIR}"
    CONFIGURE_COMMAND meson setup <BINARY_DIR> <SOURCE_DIR> --prefix=<INSTALL_DIR> ${ARG_MESON_ARGS}
    BUILD_COMMAND ninja -C <BINARY_DIR>
    INSTALL_COMMAND ninja -C <BINARY_DIR> install
    PATCH_COMMAND ${ARG_PATCH_COMMAND}
    BUILD_BYPRODUCTS ${ARG_BUILD_BYPRODUCTS}
    EXTRA_DEPENDS ${ARG_EXTRA_DEPENDS})
endfunction()

function(kodi_dep_script)
  set(options)
  set(oneValueArgs NAME PACKAGE PACKAGE_TYPE SOURCE_DIR BINARY_DIR INSTALL_DIR CONFIGURE_COMMAND BUILD_COMMAND INSTALL_COMMAND)
  set(multiValueArgs EXTRA_DEPENDS PATCH_COMMAND BUILD_BYPRODUCTS)
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT ARG_NAME)
    set(ARG_NAME "kodi-depends-${ARG_PACKAGE_TYPE}-${ARG_PACKAGE}")
  endif()

  kodi_depends_add_external_recipe(
    NAME "${ARG_NAME}"
    PACKAGE "${ARG_PACKAGE}"
    PACKAGE_TYPE "${ARG_PACKAGE_TYPE}"
    SOURCE_DIR "${ARG_SOURCE_DIR}"
    BINARY_DIR "${ARG_BINARY_DIR}"
    INSTALL_DIR "${ARG_INSTALL_DIR}"
    CONFIGURE_COMMAND ${ARG_CONFIGURE_COMMAND}
    BUILD_COMMAND ${ARG_BUILD_COMMAND}
    INSTALL_COMMAND ${ARG_INSTALL_COMMAND}
    PATCH_COMMAND ${ARG_PATCH_COMMAND}
    BUILD_BYPRODUCTS ${ARG_BUILD_BYPRODUCTS}
    EXTRA_DEPENDS ${ARG_EXTRA_DEPENDS})
endfunction()
