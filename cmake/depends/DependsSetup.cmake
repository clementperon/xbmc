include_guard(GLOBAL)

set(KODI_DEPENDS_SOURCE_DIR "${CMAKE_SOURCE_DIR}/tools/depends" CACHE PATH
    "Path to tools/depends source directory")
set(KODI_DEPENDS_PREFIX "${CMAKE_BINARY_DIR}/depends" CACHE PATH
    "Depends install root passed to tools/depends --prefix")
set(KODI_DEPENDS_TARBALLS "" CACHE PATH
    "Optional tarballs cache path passed to tools/depends --with-tarballs")
set(KODI_DEPENDS_HOST "" CACHE STRING
    "tools/depends --host value (required for depends bootstrap)")
set(KODI_DEPENDS_TARGET_PLATFORM "" CACHE STRING
    "tools/depends --with-platform value")
set(KODI_DEPENDS_RENDER_SYSTEM "gl" CACHE STRING
    "tools/depends --with-rendersystem value")
set(KODI_DEPENDS_TOOLCHAIN "" CACHE PATH
    "tools/depends --with-toolchain path")
set(KODI_DEPENDS_SDK_PATH "" CACHE PATH
    "tools/depends --with-sdk-path path")
set(KODI_DEPENDS_NDK_PATH "" CACHE PATH
    "tools/depends --with-ndk-path path")
set(KODI_DEPENDS_NDK_API "" CACHE STRING
    "tools/depends --with-ndk-api value")
set(KODI_DEPENDS_SDK_VERSION "" CACHE STRING
    "tools/depends --with-sdk value")
set(KODI_DEPENDS_DEBUG OFF CACHE BOOL
    "Enable debug tools/depends configuration")
set(KODI_DEPENDS_ENABLE_GPLV3 ON CACHE BOOL
    "Enable GPLv3 target dependencies in CMake depends graph")
set(KODI_DEPENDS_HAVE_ZLIB "0" CACHE STRING
    "Set to 1 if target zlib is available externally")
set(KODI_DEPENDS_NEED_LIBICONV "1" CACHE STRING
    "Set to 1 if target libiconv must be built")
set(KODI_DEPENDS_EXTRA_CONFIGURE_ARGS "" CACHE STRING
    "Additional arguments forwarded to tools/depends configure")

function(kodi_depends_get_debug_switch out_var)
  if(KODI_DEPENDS_DEBUG)
    set(${out_var} "" PARENT_SCOPE)
  else()
    set(${out_var} "--disable-debug" PARENT_SCOPE)
  endif()
endfunction()

function(kodi_depends_get_configure_args out_var)
  if(NOT KODI_DEPENDS_HOST)
    message(FATAL_ERROR "KODI_DEPENDS_HOST must be set when using CMake-native depends bootstrap.")
  endif()

  kodi_depends_get_debug_switch(_debug_switch)

  set(_args --host=${KODI_DEPENDS_HOST}
            --prefix=${KODI_DEPENDS_PREFIX}
            --with-rendersystem=${KODI_DEPENDS_RENDER_SYSTEM})

  if(KODI_DEPENDS_TARGET_PLATFORM)
    list(APPEND _args --with-platform=${KODI_DEPENDS_TARGET_PLATFORM})
  endif()
  if(KODI_DEPENDS_TARBALLS)
    list(APPEND _args --with-tarballs=${KODI_DEPENDS_TARBALLS})
  endif()
  if(KODI_DEPENDS_TOOLCHAIN)
    list(APPEND _args --with-toolchain=${KODI_DEPENDS_TOOLCHAIN})
  endif()
  if(KODI_DEPENDS_SDK_PATH)
    list(APPEND _args --with-sdk-path=${KODI_DEPENDS_SDK_PATH})
  endif()
  if(KODI_DEPENDS_NDK_PATH)
    list(APPEND _args --with-ndk-path=${KODI_DEPENDS_NDK_PATH})
  endif()
  if(KODI_DEPENDS_NDK_API)
    list(APPEND _args --with-ndk-api=${KODI_DEPENDS_NDK_API})
  endif()
  if(KODI_DEPENDS_SDK_VERSION)
    list(APPEND _args --with-sdk=${KODI_DEPENDS_SDK_VERSION})
  endif()
  if(_debug_switch)
    list(APPEND _args ${_debug_switch})
  endif()

  if(KODI_DEPENDS_EXTRA_CONFIGURE_ARGS)
    separate_arguments(_extra_args UNIX_COMMAND "${KODI_DEPENDS_EXTRA_CONFIGURE_ARGS}")
    list(APPEND _args ${_extra_args})
  endif()

  set(${out_var} "${_args}" PARENT_SCOPE)
endfunction()

function(kodi_depends_read_makefile_include makefile_include_path)
  if(NOT EXISTS "${makefile_include_path}")
    message(FATAL_ERROR "Missing generated Makefile.include: ${makefile_include_path}")
  endif()

  file(STRINGS "${makefile_include_path}" _prefix_line REGEX "^PREFIX=" LIMIT_COUNT 1)
  file(STRINGS "${makefile_include_path}" _nativeprefix_line REGEX "^NATIVEPREFIX=" LIMIT_COUNT 1)
  file(STRINGS "${makefile_include_path}" _platform_line REGEX "^PLATFORM=" LIMIT_COUNT 1)
  file(STRINGS "${makefile_include_path}" _os_line REGEX "^OS=" LIMIT_COUNT 1)
  file(STRINGS "${makefile_include_path}" _target_platform_line REGEX "^TARGET_PLATFORM=" LIMIT_COUNT 1)
  file(STRINGS "${makefile_include_path}" _cpu_line REGEX "^CPU=" LIMIT_COUNT 1)

  string(REGEX REPLACE "^PREFIX=(.*)$" "\\1" KODI_DEPENDS_PREFIX_RESOLVED "${_prefix_line}")
  string(REGEX REPLACE "^NATIVEPREFIX=(.*)$" "\\1" KODI_DEPENDS_NATIVEPREFIX_RESOLVED "${_nativeprefix_line}")
  string(REGEX REPLACE "^PLATFORM=(.*)$" "\\1" KODI_DEPENDS_PLATFORM_RESOLVED "${_platform_line}")
  string(REGEX REPLACE "^OS=(.*)$" "\\1" KODI_DEPENDS_OS "${_os_line}")
  string(REGEX REPLACE "^TARGET_PLATFORM=(.*)$" "\\1" KODI_DEPENDS_TARGET_PLATFORM "${_target_platform_line}")
  string(REGEX REPLACE "^CPU=(.*)$" "\\1" KODI_DEPENDS_CPU "${_cpu_line}")

  set(KODI_DEPENDS_PREFIX_RESOLVED "${KODI_DEPENDS_PREFIX_RESOLVED}" PARENT_SCOPE)
  set(KODI_DEPENDS_NATIVEPREFIX_RESOLVED "${KODI_DEPENDS_NATIVEPREFIX_RESOLVED}" PARENT_SCOPE)
  set(KODI_DEPENDS_PLATFORM_RESOLVED "${KODI_DEPENDS_PLATFORM_RESOLVED}" PARENT_SCOPE)
  set(KODI_DEPENDS_OS "${KODI_DEPENDS_OS}" PARENT_SCOPE)
  set(KODI_DEPENDS_TARGET_PLATFORM "${KODI_DEPENDS_TARGET_PLATFORM}" PARENT_SCOPE)
  set(KODI_DEPENDS_CPU "${KODI_DEPENDS_CPU}" PARENT_SCOPE)
endfunction()
