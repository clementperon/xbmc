#.rst:
# FindEGL
# -------
# Finds the EGL library
#
# This will define the following target:
#
#   ${APP_NAME_LC}::EGL   - The EGL library

if(NOT TARGET ${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME})
  set(_egl_target ${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME})
  find_package(PkgConfig ${SEARCH_QUIET})
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_EGL egl ${SEARCH_QUIET})
  endif()

  find_path(EGL_INCLUDE_DIR EGL/egl.h
                            HINTS ${PC_EGL_INCLUDEDIR})

  if(NOT EGL_LIBRARY)
    find_library(EGL_LIBRARY NAMES EGL egl
                             HINTS ${PC_EGL_LIBDIR})
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten" AND NOT EGL_LIBRARY)
    # Emscripten exposes EGL as a link name instead of a filesystem library.
    set(EGL_LIBRARY EGL)
  endif()

  set(EGL_VERSION ${PC_EGL_VERSION})

  if(NOT VERBOSE_FIND)
     set(${CMAKE_FIND_PACKAGE_NAME}_FIND_QUIETLY TRUE)
   endif()

  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(EGL
                                    REQUIRED_VARS EGL_LIBRARY EGL_INCLUDE_DIR
                                    VERSION_VAR EGL_VERSION)

  if(EGL_FOUND)
    list(APPEND GL_INTERFACES_LIST egl egl-pb)
    set(GL_INTERFACES_LIST ${GL_INTERFACES_LIST} PARENT_SCOPE)

    set(CMAKE_REQUIRED_INCLUDES "${EGL_INCLUDE_DIR}")
    include(CheckIncludeFiles)
    check_include_files("EGL/egl.h;EGL/eglext.h;EGL/eglext_angle.h" HAVE_EGLEXTANGLE)
    unset(CMAKE_REQUIRED_INCLUDES)

    if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
      add_library(${_egl_target} INTERFACE IMPORTED)
      set_target_properties(${_egl_target} PROPERTIES
                                        INTERFACE_LINK_LIBRARIES "${EGL_LIBRARY}"
                                        INTERFACE_INCLUDE_DIRECTORIES "${EGL_INCLUDE_DIR}"
                                        INTERFACE_COMPILE_DEFINITIONS HAS_EGL)
    elseif(${EGL_LIBRARY} MATCHES ".+\.so$")
      add_library(${_egl_target} SHARED IMPORTED)
    else()
      add_library(${_egl_target} UNKNOWN IMPORTED)
    endif()

    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
      set_target_properties(${_egl_target} PROPERTIES
                                          IMPORTED_LOCATION "${EGL_LIBRARY}"
                                          INTERFACE_INCLUDE_DIRECTORIES "${EGL_INCLUDE_DIR}"
                                          INTERFACE_COMPILE_DEFINITIONS HAS_EGL
                                          IMPORTED_NO_SONAME TRUE)
    endif()

    if(HAVE_EGLEXTANGLE)
      set_property(TARGET ${_egl_target} APPEND PROPERTY
                                                                            INTERFACE_COMPILE_DEFINITIONS HAVE_EGLEXTANGLE)
    endif()
  endif()
endif()
