cmake_minimum_required(VERSION 3.18)
project(kodi-depends-superbuild LANGUAGES NONE)

include(ExternalProject)
include("${CMAKE_SOURCE_DIR}/cmake/depends/DependsSetup.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/depends/DependsGraph.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/depends/DependsRecipeHelpers.cmake")

kodi_depends_get_configure_args(KODI_DEPENDS_CONFIGURE_ARGS)

set(_depends_makefile_include "${KODI_DEPENDS_SOURCE_DIR}/Makefile.include")
set(_depends_configure_stamp "${CMAKE_BINARY_DIR}/depends/.kodi_depends_configured")

add_custom_target(kodi-depends-bootstrap
  COMMAND ${CMAKE_COMMAND} -E chdir "${KODI_DEPENDS_SOURCE_DIR}" ./bootstrap
  WORKING_DIRECTORY "${KODI_DEPENDS_SOURCE_DIR}"
  USES_TERMINAL
  COMMENT "Bootstrapping tools/depends")

add_custom_command(
  OUTPUT "${_depends_configure_stamp}"
  COMMAND ${CMAKE_COMMAND} -E chdir "${KODI_DEPENDS_SOURCE_DIR}" ./configure ${KODI_DEPENDS_CONFIGURE_ARGS}
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/depends"
  COMMAND ${CMAKE_COMMAND} -E touch "${_depends_configure_stamp}"
  DEPENDS kodi-depends-bootstrap
  WORKING_DIRECTORY "${KODI_DEPENDS_SOURCE_DIR}"
  COMMENT "Configuring tools/depends")

add_custom_target(kodi-depends-configure DEPENDS "${_depends_configure_stamp}")

# Prime legacy metadata so package selection mirrors tools/depends Makefiles.
execute_process(
  COMMAND ./bootstrap
  WORKING_DIRECTORY "${KODI_DEPENDS_SOURCE_DIR}"
  RESULT_VARIABLE _depends_bootstrap_rc
)
if(NOT _depends_bootstrap_rc EQUAL 0)
  message(FATAL_ERROR "Failed to bootstrap tools/depends for superbuild graph generation.")
endif()
execute_process(
  COMMAND ./configure ${KODI_DEPENDS_CONFIGURE_ARGS}
  WORKING_DIRECTORY "${KODI_DEPENDS_SOURCE_DIR}"
  RESULT_VARIABLE _depends_configure_rc
)
if(NOT _depends_configure_rc EQUAL 0)
  message(FATAL_ERROR "Failed to configure tools/depends for superbuild graph generation.")
endif()
kodi_depends_read_makefile_include("${_depends_makefile_include}")

kodi_depends_get_native_packages(_native_packages)
kodi_depends_get_target_packages(_target_packages)

foreach(_pkg IN LISTS _native_packages)
  kodi_depends_pair_dependencies("${KODI_DEPENDS_NATIVE_DEPENDENCY_PAIRS}" "${_pkg}" _pkg_deps)
  set(_dep_targets "")
  foreach(_dep IN LISTS _pkg_deps)
    if(_dep IN_LIST _native_packages)
      list(APPEND _dep_targets "kodi-depends-native-${_dep}")
    endif()
  endforeach()
  kodi_depends_add_make_target(
    NAME "kodi-depends-native-${_pkg}"
    PACKAGE "${_pkg}"
    PACKAGE_TYPE "native"
    EXTRA_DEPENDS "${_dep_targets}")
endforeach()

foreach(_pkg IN LISTS _target_packages)
  kodi_depends_pair_dependencies("${KODI_DEPENDS_TARGET_DEPENDENCY_PAIRS}" "${_pkg}" _pkg_deps)
  set(_dep_targets "")
  foreach(_dep IN LISTS _pkg_deps)
    if(_dep IN_LIST _target_packages)
      list(APPEND _dep_targets "kodi-depends-target-${_dep}")
    elseif(_dep IN_LIST _native_packages)
      list(APPEND _dep_targets "kodi-depends-native-${_dep}")
    endif()
  endforeach()
  kodi_depends_add_make_target(
    NAME "kodi-depends-target-${_pkg}"
    PACKAGE "${_pkg}"
    PACKAGE_TYPE "target"
    EXTRA_DEPENDS "${_dep_targets}")
endforeach()

add_custom_target(kodi-depends-native)
foreach(_pkg IN LISTS _native_packages)
  add_dependencies(kodi-depends-native "kodi-depends-native-${_pkg}")
endforeach()

add_custom_target(kodi-depends-target)
add_dependencies(kodi-depends-target kodi-depends-native)
foreach(_pkg IN LISTS _target_packages)
  add_dependencies(kodi-depends-target "kodi-depends-target-${_pkg}")
endforeach()

add_custom_target(kodi-depends)
add_dependencies(kodi-depends kodi-depends-target)

set(_depends_toolchain "${KODI_DEPENDS_PREFIX_RESOLVED}/share/Toolchain.cmake")

set(_kodi_cmake_args
  -DKODI_SUPERBUILD_DEPENDS=OFF
  -DCMAKE_TOOLCHAIN_FILE=${_depends_toolchain}
)
if(CMAKE_BUILD_TYPE)
  list(APPEND _kodi_cmake_args "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
endif()
ExternalProject_Add(kodi-core
  SOURCE_DIR "${CMAKE_SOURCE_DIR}"
  BINARY_DIR "${CMAKE_BINARY_DIR}/kodi"
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  CMAKE_ARGS ${_kodi_cmake_args}
  BUILD_COMMAND ${CMAKE_COMMAND} --build .
  INSTALL_COMMAND ""
  DEPENDS kodi-depends
)
