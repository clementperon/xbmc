set(SOURCES WinEventsWasm.cpp
            WasmClipboard.cpp
            WasmKeyboard.cpp)

set(HEADERS WinEventsWasm.h
            WasmClipboard.h
            WasmKeyboard.h)

if(TARGET ${APP_NAME_LC}::OpenGLES)
  list(APPEND SOURCES WinSystemWasmGLESContext.cpp)
  list(APPEND HEADERS WinSystemWasmGLESContext.h)
endif()

core_add_library(windowing_wasm)
