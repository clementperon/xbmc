# Generic depends-driven toolchain template for CMake-native depends bootstrap.
# This file is intended as a stable replacement entrypoint for generated
# tools/depends Toolchain.cmake where possible.

if(NOT DEFINED DEPENDS_PATH)
  message(FATAL_ERROR "DEPENDS_PATH must be set for KodiDependsToolchain.cmake")
endif()

if(NOT DEFINED NATIVEPREFIX)
  message(FATAL_ERROR "NATIVEPREFIX must be set for KodiDependsToolchain.cmake")
endif()

if(NOT DEFINED KODI_DEPENDS_OS)
  set(KODI_DEPENDS_OS linux)
endif()

if(KODI_DEPENDS_OS STREQUAL "linux")
  set(CMAKE_SYSTEM_NAME Linux)
  set(CORE_SYSTEM_NAME linux)
elseif(KODI_DEPENDS_OS STREQUAL "android")
  set(CMAKE_SYSTEM_NAME Android)
  set(CORE_SYSTEM_NAME android)
elseif(KODI_DEPENDS_OS STREQUAL "osx")
  set(CMAKE_SYSTEM_NAME Darwin)
  set(CORE_SYSTEM_NAME osx)
elseif(KODI_DEPENDS_OS STREQUAL "darwin_embedded")
  set(CMAKE_SYSTEM_NAME Darwin)
  set(CORE_SYSTEM_NAME darwin_embedded)
elseif(KODI_DEPENDS_OS STREQUAL "wasm")
  set(CMAKE_SYSTEM_NAME Emscripten)
  set(CORE_SYSTEM_NAME wasm)
endif()

set(KODI_DEPENDSBUILD 1)
set(CMAKE_FIND_ROOT_PATH "${DEPENDS_PATH}")
set(CMAKE_LIBRARY_PATH "${DEPENDS_PATH}/lib")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_FRAMEWORK LAST)

set(ENV{PKG_CONFIG_LIBDIR} "${DEPENDS_PATH}/lib/pkgconfig:${DEPENDS_PATH}/share/pkgconfig")
set(ENV{ACLOCAL_PATH} "${DEPENDS_PATH}/share/aclocal:${NATIVEPREFIX}/share/aclocal")
set(ENV{PATH} "${NATIVEPREFIX}/bin:$ENV{PATH}")

find_file(CONFIG_SITE "config.site" PATHS "${DEPENDS_PATH}/share" NO_CMAKE_FIND_ROOT_PATH)
if(CONFIG_SITE)
  set(ENV{CONFIG_SITE} "${CONFIG_SITE}")
endif()
