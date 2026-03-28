# WebAssembly (Emscripten) architecture setup
include(cmake/scripts/wasm/Linkers.txt)

set(CORE_MAIN_SOURCE ${CMAKE_SOURCE_DIR}/xbmc/platform/wasm/main.cpp)

list(APPEND ARCH_DEFINES -DTARGET_POSIX -DTARGET_WASM)
set(SYSTEM_DEFINES -D__STDC_CONSTANT_MACROS -D_FILE_OFFSET_BITS=64)
set(PLATFORM_DIR platform/wasm)
set(PLATFORMDEFS_DIR platform/posix)

# Emscripten identifies as Unix; keep POSIX networking defines where applicable
list(APPEND SYSTEM_DEFINES -DHAS_POSIX_NETWORK)

set(CMAKE_SYSTEM_NAME Emscripten)

if(WITH_ARCH)
  set(ARCH ${WITH_ARCH})
else()
  set(ARCH wasm32-unknown-emscripten)
endif()

if(WITH_CPU)
  set(CPU ${WITH_CPU})
else()
  set(CPU wasm32)
endif()

set(NEON False)

# Cannot run unit tests on host
set(HOST_CAN_EXECUTE_TARGET FALSE)

# Prefer internal libs when cross-compiling to wasm (typical for depends builds)
if(NOT USE_INTERNAL_LIBS)
  if(KODI_DEPENDSBUILD OR CMAKE_CROSSCOMPILING)
    set(USE_INTERNAL_LIBS ON)
  else()
    set(USE_INTERNAL_LIBS ON)
  endif()
endif()

list(APPEND AUDIO_BACKENDS_LIST "webaudio")

set(APP_BINARY_SUFFIX ".js")

# Threading + memory (COOP/COEP headers required in HTML for pthreads)
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  add_link_options(
    "SHELL:-pthread"
    "SHELL:-sUSE_PTHREADS=1"
    "SHELL:-sPTHREAD_POOL_SIZE=8"
    "SHELL:-sALLOW_MEMORY_GROWTH=1"
    "SHELL:-sINITIAL_MEMORY=256MB"
    "SHELL:-sMAXIMUM_MEMORY=4GB"
    "SHELL:-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap']"
    "SHELL:-sEXPORTED_FUNCTIONS=['_main','_malloc','_free']"
    "SHELL:-sPROXY_TO_PTHREAD"
    "SHELL:-sOFFSCREEN_FRAMEBUFFER"
    "SHELL:-sMIN_WEBGL_VERSION=2"
    "SHELL:-sMAX_WEBGL_VERSION=2"
    "SHELL:-sFULL_ES3=1"
    "SHELL:-sABORTING_MALLOC=0"
    "SHELL:-sASSERTIONS=1"
    "SHELL:-lidbfs.js"
  )
  add_compile_options(-pthread)
endif()

# Preload Kodi data directories into the Emscripten virtual filesystem under /kodi.
# The data is copied into the build tree by cmake install rules; we reference it from there.
set(WASM_PRELOAD_ROOT "${CMAKE_BINARY_DIR}")
set(WASM_VFS_PREFIX "/kodi")
foreach(_dir addons media system userdata)
  add_link_options("SHELL:--preload-file ${WASM_PRELOAD_ROOT}/${_dir}@${WASM_VFS_PREFIX}/${_dir}")
endforeach()

