include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/DependsInventory.cmake")

function(kodi_depends_get_native_packages out_var)
  if(KODI_DEPENDS_OS STREQUAL "windows")
    set(_pkgs ${KODI_DEPENDS_NATIVE_WINDOWS_MINIMAL})
    list(REMOVE_DUPLICATES _pkgs)
    set(${out_var} "${_pkgs}" PARENT_SCOPE)
    return()
  endif()

  set(_pkgs ${KODI_DEPENDS_NATIVE_BASE})

  if(NOT KODI_DEPENDS_OS STREQUAL "osx" AND NOT KODI_DEPENDS_OS STREQUAL "darwin_embedded")
    list(APPEND _pkgs ${KODI_DEPENDS_NATIVE_NOT_OSX})
  endif()

  if(KODI_DEPENDS_OS STREQUAL "darwin_embedded")
    list(APPEND _pkgs ${KODI_DEPENDS_NATIVE_DARWIN_EMBEDDED})
  endif()

  if(KODI_DEPENDS_OS STREQUAL "linux")
    list(APPEND _pkgs ${KODI_DEPENDS_NATIVE_LINUX})
    if(KODI_DEPENDS_RENDER_SYSTEM STREQUAL "gles")
      list(APPEND _pkgs ${KODI_DEPENDS_NATIVE_LINUX_GLES})
    endif()
    if(KODI_DEPENDS_TARGET_PLATFORM STREQUAL "webos")
      list(APPEND _pkgs ${KODI_DEPENDS_NATIVE_LINUX_WEBOS})
    endif()
  endif()

  if(KODI_DEPENDS_OS STREQUAL "android")
    list(APPEND _pkgs ${KODI_DEPENDS_NATIVE_ANDROID})
  endif()

  list(REMOVE_DUPLICATES _pkgs)
  set(${out_var} "${_pkgs}" PARENT_SCOPE)
endfunction()

function(kodi_depends_get_target_packages out_var)
  if(KODI_DEPENDS_OS STREQUAL "windows")
    if(KODI_DEPENDS_WINDOWS_PROFILE STREQUAL "expanded")
      set(_pkgs ${KODI_DEPENDS_TARGET_WINDOWS_EXPANDED})
    else()
      set(_pkgs ${KODI_DEPENDS_TARGET_WINDOWS_MINIMAL})
    endif()

    if(KODI_DEPENDS_HAVE_ZLIB STREQUAL "0")
      list(APPEND _pkgs ${KODI_DEPENDS_TARGET_ZLIB})
    endif()
    if(KODI_DEPENDS_NEED_LIBICONV STREQUAL "1")
      list(APPEND _pkgs ${KODI_DEPENDS_TARGET_ICONV})
    endif()

    list(REMOVE_DUPLICATES _pkgs)
    set(${out_var} "${_pkgs}" PARENT_SCOPE)
    return()
  endif()

  set(_pkgs ${KODI_DEPENDS_TARGET_BASE})

  if(KODI_DEPENDS_ENABLE_GPLV3)
    list(APPEND _pkgs ${KODI_DEPENDS_TARGET_GPLV3})
  else()
    list(APPEND _pkgs ${KODI_DEPENDS_TARGET_NON_GPLV3})
  endif()

  if(KODI_DEPENDS_OS STREQUAL "darwin_embedded")
    list(APPEND _pkgs ${KODI_DEPENDS_TARGET_DARWIN_EMBEDDED_ADD})
    list(REMOVE_ITEM _pkgs ${KODI_DEPENDS_TARGET_DARWIN_EMBEDDED_EXCLUDE})
    if(KODI_DEPENDS_TARGET_PLATFORM STREQUAL "appletvos")
      list(REMOVE_ITEM _pkgs ${KODI_DEPENDS_TARGET_TVOS_EXTRA_EXCLUDE})
    endif()
  endif()

  if(KODI_DEPENDS_OS STREQUAL "osx")
    list(APPEND _pkgs ${KODI_DEPENDS_TARGET_OSX_ADD})
    list(REMOVE_ITEM _pkgs ${KODI_DEPENDS_TARGET_OSX_EXCLUDE})
  endif()

  if(KODI_DEPENDS_OS STREQUAL "android")
    list(APPEND _pkgs ${KODI_DEPENDS_TARGET_ANDROID_ADD})
    list(REMOVE_ITEM _pkgs ${KODI_DEPENDS_TARGET_ANDROID_EXCLUDE})
  endif()

  if(KODI_DEPENDS_OS STREQUAL "wasm")
    list(APPEND _pkgs ${KODI_DEPENDS_TARGET_WASM_ADD})
    list(REMOVE_ITEM _pkgs ${KODI_DEPENDS_TARGET_WASM_EXCLUDE})
  endif()

  if(KODI_DEPENDS_HAVE_ZLIB STREQUAL "0")
    list(APPEND _pkgs ${KODI_DEPENDS_TARGET_ZLIB})
  endif()

  if(KODI_DEPENDS_NEED_LIBICONV STREQUAL "1")
    list(APPEND _pkgs ${KODI_DEPENDS_TARGET_ICONV})
  endif()

  if(KODI_DEPENDS_OS STREQUAL "linux")
    list(APPEND _pkgs ${KODI_DEPENDS_TARGET_LINUX_ADD})

    if(KODI_DEPENDS_CPU STREQUAL "x86_64")
      list(APPEND _pkgs ${KODI_DEPENDS_TARGET_LINUX_X86_64_ADD})
    endif()

    if(KODI_DEPENDS_TARGET_PLATFORM MATCHES "wayland")
      list(APPEND _pkgs ${KODI_DEPENDS_TARGET_LINUX_WAYLAND_ADD})
    endif()

    if(KODI_DEPENDS_TARGET_PLATFORM MATCHES "x11")
      list(APPEND _pkgs ${KODI_DEPENDS_TARGET_LINUX_X11_ADD})
    endif()

    if(KODI_DEPENDS_RENDER_SYSTEM STREQUAL "gl")
      list(APPEND _pkgs ${KODI_DEPENDS_TARGET_LINUX_GL_ADD})
    else()
      list(APPEND _pkgs ${KODI_DEPENDS_TARGET_LINUX_GLES_ADD})
    endif()

    if(KODI_DEPENDS_TARGET_PLATFORM STREQUAL "webos")
      list(APPEND _pkgs ${KODI_DEPENDS_TARGET_WEBOS_ADD})
      list(REMOVE_ITEM _pkgs ${KODI_DEPENDS_TARGET_WEBOS_EXCLUDE})
    endif()

    if(KODI_DEPENDS_TARGET_PLATFORM MATCHES "gbm")
      list(APPEND _pkgs ${KODI_DEPENDS_TARGET_GBM_ADD})
    endif()
  endif()

  list(REMOVE_DUPLICATES _pkgs)
  set(${out_var} "${_pkgs}" PARENT_SCOPE)
endfunction()
