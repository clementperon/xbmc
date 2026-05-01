cmake_minimum_required(VERSION 3.18)
project(kodi-depends-superbuild LANGUAGES NONE)

include(ExternalProject)
include("${CMAKE_SOURCE_DIR}/cmake/depends/DependsSetup.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/depends/DependsGraph.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/depends/DependsRecipeHelpers.cmake")

if(WIN32 AND KODI_SUPERBUILD_WINDOWS_EXPERIMENTAL)
  if(NOT KODI_DEPENDS_HOST)
    if(KODI_SUPERBUILD_WINDOWS_ARCH STREQUAL "win32")
      set(KODI_DEPENDS_HOST "i686-w64-mingw32" CACHE STRING
          "tools/depends --host value (required for depends bootstrap)" FORCE)
    elseif(KODI_SUPERBUILD_WINDOWS_ARCH STREQUAL "arm64")
      set(KODI_DEPENDS_HOST "aarch64-w64-mingw32" CACHE STRING
          "tools/depends --host value (required for depends bootstrap)" FORCE)
    else()
      set(KODI_DEPENDS_HOST "x86_64-w64-mingw32" CACHE STRING
          "tools/depends --host value (required for depends bootstrap)" FORCE)
    endif()
  endif()

  if(NOT KODI_DEPENDS_TARGET_PLATFORM)
    if(KODI_SUPERBUILD_WINDOWS_UWP)
      set(KODI_DEPENDS_TARGET_PLATFORM "windowsstore" CACHE STRING
          "tools/depends --with-platform value" FORCE)
    else()
      set(KODI_DEPENDS_TARGET_PLATFORM "windows" CACHE STRING
          "tools/depends --with-platform value" FORCE)
    endif()
  endif()
endif()

kodi_depends_get_configure_args(KODI_DEPENDS_CONFIGURE_ARGS)

set(_depends_makefile_include "${KODI_DEPENDS_SOURCE_DIR}/Makefile.include")
set(_depends_configure_stamp "${CMAKE_BINARY_DIR}/depends/.kodi_depends_configured")
string(REPLACE ";" " " _depends_configure_args_string "${KODI_DEPENDS_CONFIGURE_ARGS}")

if(WIN32)
  file(TO_CMAKE_PATH "${KODI_DEPENDS_SOURCE_DIR}" _depends_source_dir_msys)
  kodi_depends_get_shell_command(_depends_bootstrap_cmd "cd \"${_depends_source_dir_msys}\" && ./bootstrap")
  kodi_depends_get_shell_command(_depends_configure_cmd
                                 "cd \"${_depends_source_dir_msys}\" && ./configure ${_depends_configure_args_string}")
else()
  set(_depends_bootstrap_cmd ${CMAKE_COMMAND} -E chdir "${KODI_DEPENDS_SOURCE_DIR}" ./bootstrap)
  set(_depends_configure_cmd ${CMAKE_COMMAND} -E chdir "${KODI_DEPENDS_SOURCE_DIR}" ./configure ${KODI_DEPENDS_CONFIGURE_ARGS})
endif()

add_custom_target(kodi-depends-bootstrap
  COMMAND ${_depends_bootstrap_cmd}
  WORKING_DIRECTORY "${KODI_DEPENDS_SOURCE_DIR}"
  USES_TERMINAL
  COMMENT "Bootstrapping tools/depends")

add_custom_command(
  OUTPUT "${_depends_configure_stamp}"
  COMMAND ${_depends_configure_cmd}
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/depends"
  COMMAND ${CMAKE_COMMAND} -E touch "${_depends_configure_stamp}"
  DEPENDS kodi-depends-bootstrap
  WORKING_DIRECTORY "${KODI_DEPENDS_SOURCE_DIR}"
  COMMENT "Configuring tools/depends")

add_custom_target(kodi-depends-configure DEPENDS "${_depends_configure_stamp}")

# Prime legacy metadata so package selection mirrors tools/depends Makefiles.
execute_process(
  COMMAND ${_depends_bootstrap_cmd}
  WORKING_DIRECTORY "${KODI_DEPENDS_SOURCE_DIR}"
  RESULT_VARIABLE _depends_bootstrap_rc
)
if(NOT _depends_bootstrap_rc EQUAL 0)
  message(FATAL_ERROR "Failed to bootstrap tools/depends for superbuild graph generation.")
endif()
execute_process(
  COMMAND ${_depends_configure_cmd}
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
)
set(_kodi_generator_platform "")
set(_kodi_generator_toolset "")
if(WIN32 AND KODI_SUPERBUILD_WINDOWS_EXPERIMENTAL)
  if(KODI_SUPERBUILD_WINDOWS_ARCH STREQUAL "win32")
    set(_kodi_generator_platform "Win32")
  elseif(KODI_SUPERBUILD_WINDOWS_ARCH STREQUAL "arm64")
    set(_kodi_generator_platform "ARM64")
  else()
    set(_kodi_generator_platform "x64")
  endif()

  if(KODI_SUPERBUILD_WINDOWS_UWP)
    list(APPEND _kodi_cmake_args
         "-DCMAKE_SYSTEM_NAME=WindowsStore"
         "-DCMAKE_SYSTEM_VERSION=10.0")
  endif()
  if(CMAKE_GENERATOR_TOOLSET)
    set(_kodi_generator_toolset "${CMAKE_GENERATOR_TOOLSET}")
  endif()
else()
  list(APPEND _kodi_cmake_args "-DCMAKE_TOOLCHAIN_FILE=${_depends_toolchain}")
endif()
if(CMAKE_BUILD_TYPE)
  list(APPEND _kodi_cmake_args "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
endif()
set(_kodi_core_externalproject_args
  SOURCE_DIR "${CMAKE_SOURCE_DIR}"
  BINARY_DIR "${CMAKE_BINARY_DIR}/kodi"
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  CMAKE_ARGS ${_kodi_cmake_args}
  BUILD_COMMAND ${CMAKE_COMMAND} --build .
  INSTALL_COMMAND ""
  DEPENDS kodi-depends
)
if(_kodi_generator_platform)
  list(APPEND _kodi_core_externalproject_args CMAKE_GENERATOR_PLATFORM "${_kodi_generator_platform}")
endif()
if(_kodi_generator_toolset)
  list(APPEND _kodi_core_externalproject_args CMAKE_GENERATOR_TOOLSET "${_kodi_generator_toolset}")
endif()
ExternalProject_Add(kodi-core ${_kodi_core_externalproject_args})

add_custom_target(kodi-e2e)
add_dependencies(kodi-e2e kodi-core)

if(KODI_SUPERBUILD_ADDONS)
  kodi_depends_get_addons_package_zip(_kodi_addons_package_zip)

  if(KODI_ADDONS_INSTALL_PREFIX)
    set(_kodi_addons_install_prefix "${KODI_ADDONS_INSTALL_PREFIX}")
  elseif(KODI_DEPENDS_OS STREQUAL "android")
    set(_kodi_addons_install_prefix "${CMAKE_BINARY_DIR}/kodi")
  else()
    set(_kodi_addons_install_prefix "${CMAKE_BINARY_DIR}/addons")
  endif()

  set(_kodi_addons_build_dir "${CMAKE_BINARY_DIR}/addons-work")
  set(_kodi_addons_binary_dir "${CMAKE_BINARY_DIR}/addons-build")
  set(_kodi_addons_depends_target kodi-core)
  set(_kodi_addons_autoconf_files "${KODI_ADDONS_AUTOCONF_FILES}")
  if(NOT _kodi_addons_autoconf_files
     AND EXISTS "${KODI_DEPENDS_SOURCE_DIR}/target/config.sub"
     AND EXISTS "${KODI_DEPENDS_SOURCE_DIR}/target/config.guess"
     AND (NOT KODI_DEPENDS_OS STREQUAL "linux" OR KODI_DEPENDS_TARGET_PLATFORM))
    set(_kodi_addons_autoconf_files
        "${KODI_DEPENDS_SOURCE_DIR}/target/config.sub ${KODI_DEPENDS_SOURCE_DIR}/target/config.guess")
  endif()

  if(KODI_DEPENDS_OS STREQUAL "linux")
    add_custom_target(kodi-addons-linux-system-libs
      COMMAND ${CMAKE_COMMAND}
              -DKODI_DEPENDS_PREFIX_RESOLVED=${KODI_DEPENDS_PREFIX_RESOLVED}
              -DKODI_DEPENDS_HOST=${KODI_DEPENDS_HOST}
              -DKODI_DEPENDS_RENDER_SYSTEM=${KODI_DEPENDS_RENDER_SYSTEM}
              -P "${CMAKE_SOURCE_DIR}/cmake/depends/EnsureLinuxAddonSystemLibs.cmake"
      COMMENT "Preparing Linux system library links for add-on build")
    add_dependencies(kodi-addons-linux-system-libs kodi-depends)
    set(_kodi_addons_depends_target kodi-addons-linux-system-libs)
  endif()

  set(_kodi_addons_cmake_args
    -DCORE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DBUILD_DIR=${_kodi_addons_build_dir}
    -DADDON_DEPENDS_PATH=${KODI_DEPENDS_PREFIX_RESOLVED}
    -DCMAKE_TOOLCHAIN_FILE=${_depends_toolchain}
    -DCMAKE_INSTALL_PREFIX=${_kodi_addons_install_prefix}
    -DADDONS_TO_BUILD=${KODI_ADDONS_TO_BUILD}
    -DPACKAGE_ZIP=${_kodi_addons_package_zip}
  )
  if(CMAKE_BUILD_TYPE)
    list(APPEND _kodi_addons_cmake_args "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
  endif()
  if(_kodi_addons_autoconf_files)
    list(APPEND _kodi_addons_cmake_args "-DAUTOCONF_FILES=${_kodi_addons_autoconf_files}")
  endif()
  if(KODI_ADDONS_DEFINITION_DIR)
    list(APPEND _kodi_addons_cmake_args "-DADDONS_DEFINITION_DIR=${KODI_ADDONS_DEFINITION_DIR}")
  endif()
  if(KODI_ADDONS_SOURCE_PREFIX)
    list(APPEND _kodi_addons_cmake_args "-DADDON_SRC_PREFIX=${KODI_ADDONS_SOURCE_PREFIX}")
  endif()
  if(KODI_ADDONS_EXTRA_CMAKE_ARGS)
    separate_arguments(_kodi_addons_extra_args UNIX_COMMAND "${KODI_ADDONS_EXTRA_CMAKE_ARGS}")
    list(APPEND _kodi_addons_cmake_args ${_kodi_addons_extra_args})
  endif()

  ExternalProject_Add(kodi-addons
    SOURCE_DIR "${CMAKE_SOURCE_DIR}/cmake/addons"
    BINARY_DIR "${_kodi_addons_binary_dir}"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS ${_kodi_addons_cmake_args}
    BUILD_COMMAND ${CMAKE_COMMAND} --build . --target ${KODI_ADDONS_BUILD_TARGET}
    INSTALL_COMMAND ""
    DEPENDS ${_kodi_addons_depends_target}
  )

  add_custom_target(kodi-addons-package
    COMMAND ${CMAKE_COMMAND}
            -DKODI_ADDONS_BINARY_DIR=${_kodi_addons_binary_dir}
            -DKODI_ADDONS_PACKAGE_TARGET=${KODI_ADDONS_PACKAGE_TARGET}
            -DKODI_ADDONS_TO_BUILD=${KODI_ADDONS_TO_BUILD}
            -DKODI_ADDONS_PACKAGE_ZIP=${_kodi_addons_package_zip}
            -DKODI_ADDONS_RESULTS_DIR=${_kodi_addons_binary_dir}
            -P "${CMAKE_SOURCE_DIR}/cmake/depends/BuildAddons.cmake"
    DEPENDS kodi-addons
    COMMENT "Packaging selected binary add-ons")

  add_dependencies(kodi-e2e kodi-addons kodi-addons-package)
else()
  add_custom_target(kodi-addons
    COMMAND ${CMAKE_COMMAND} -E echo "kodi-addons is disabled. Reconfigure with -DKODI_SUPERBUILD_ADDONS=ON."
    COMMENT "Add-on superbuild target is disabled")
  add_custom_target(kodi-addons-package
    COMMAND ${CMAKE_COMMAND} -E echo "kodi-addons-package is disabled. Reconfigure with -DKODI_SUPERBUILD_ADDONS=ON."
    COMMENT "Add-on package target is disabled")
endif()
