set(SOURCES WinEventsWasm.cpp)

set(HEADERS WinEventsWasm.h)

if(TARGET ${APP_NAME_LC}::OpenGLES)
  list(APPEND SOURCES WinSystemWasmGLESContext.cpp)
  list(APPEND HEADERS WinSystemWasmGLESContext.h)
endif()

core_add_library(windowing_wasm)
