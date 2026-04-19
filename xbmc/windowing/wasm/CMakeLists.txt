set(SOURCES WinEventsWasm.cpp
            WasmClipboard.cpp)

set(HEADERS WinEventsWasm.h
            WasmClipboard.h)

if(TARGET ${APP_NAME_LC}::OpenGLES)
  list(APPEND SOURCES WinSystemWasmGLESContext.cpp)
  list(APPEND HEADERS WinSystemWasmGLESContext.h)
endif()

core_add_library(windowing_wasm)
