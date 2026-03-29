#.rst:
# FindOpenGLES
# ------------
# Finds the OpenGLES library
#
# This will define the following target:
#
#   ${APP_NAME_LC}::OpenGLES - The OpenGLES IMPORTED library

if(NOT TARGET ${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME})
  set(_opengles_target ${APP_NAME_LC}::${CMAKE_FIND_PACKAGE_NAME})
  find_package(PkgConfig ${SEARCH_QUIET})
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_OPENGLES glesv2 ${SEARCH_QUIET})
  endif()

  if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    find_package(OpenGL ${SEARCH_QUIET} MODULE)
    if(OPENGL_FOUND)
      set(OPENGLES_gl_LIBRARY ${OPENGL_gl_LIBRARY})
      set(OPENGLES_INCLUDE_DIR ${OPENGL_INCLUDE_DIR})
    endif()
  endif()

  if(NOT OPENGLES_gl_LIBRARY)
    find_library(OPENGLES_gl_LIBRARY NAMES GLESv2 OpenGLES
                                     HINTS ${PC_OPENGLES_LIBDIR} ${CMAKE_OSX_SYSROOT}/System/Library
                                     PATH_SUFFIXES Frameworks)
  endif()
  if(NOT OPENGLES_INCLUDE_DIR)
    find_path(OPENGLES_INCLUDE_DIR NAMES GLES2/gl2.h ES2/gl.h
                                   HINTS ${PC_OPENGLES_INCLUDEDIR} ${OPENGL_INCLUDE_DIR} ${OPENGLES_gl_LIBRARY}/Headers)
  endif()
  find_path(OPENGLES3_INCLUDE_DIR NAMES GLES3/gl3.h ES3/gl.h
                                  HINTS ${PC_OPENGLES_INCLUDEDIR} ${OPENGLES_gl_LIBRARY}/Headers)

  if(NOT VERBOSE_FIND)
     set(${CMAKE_FIND_PACKAGE_NAME}_FIND_QUIETLY TRUE)
   endif()

  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(OpenGLES
                                    REQUIRED_VARS OPENGLES_gl_LIBRARY OPENGLES_INCLUDE_DIR)

  if(OPENGLES_FOUND)
    if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten" AND TARGET OpenGL::GL)
      add_library(${_opengles_target} INTERFACE IMPORTED)
      set_target_properties(${_opengles_target} PROPERTIES
                                              INTERFACE_LINK_LIBRARIES OpenGL::GL
                                              INTERFACE_INCLUDE_DIRECTORIES "${OPENGLES_INCLUDE_DIR}")
    elseif(${OPENGLES_gl_LIBRARY} MATCHES ".+\.so$")
      add_library(${_opengles_target} SHARED IMPORTED)
    else()
      add_library(${_opengles_target} UNKNOWN IMPORTED)
    endif()

    if(NOT (CMAKE_SYSTEM_NAME STREQUAL "Emscripten" AND TARGET OpenGL::GL))
      set_target_properties(${_opengles_target} PROPERTIES
                                              IMPORTED_LOCATION "${OPENGLES_gl_LIBRARY}"
                                              INTERFACE_INCLUDE_DIRECTORIES "${OPENGLES_INCLUDE_DIR}"
                                              IMPORTED_NO_SONAME TRUE)
    endif()

    if(OPENGLES3_INCLUDE_DIR)
      set_property(TARGET ${_opengles_target} APPEND PROPERTY
                                                                            INTERFACE_INCLUDE_DIRECTORIES "${OPENGLES3_INCLUDE_DIR}")
      set_target_properties(${_opengles_target} PROPERTIES
                                                                       INTERFACE_COMPILE_DEFINITIONS HAS_GLES=3)
    else()
      set_target_properties(${_opengles_target} PROPERTIES
                                                                       INTERFACE_COMPILE_DEFINITIONS HAS_GLES=2)
    endif()
  endif()
endif()
