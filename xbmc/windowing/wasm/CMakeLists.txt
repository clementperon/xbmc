set(SOURCES VideoSyncWasm.cpp
            WinEventsWasm.cpp
            WasmClipboard.cpp
            WasmKeyboard.cpp
            WasmVsync.cpp)

set(HEADERS VideoSyncWasm.h
            WinEventsWasm.h
            WasmClipboard.h
            WasmKeyboard.h
            WasmVsync.h)

if(TARGET ${APP_NAME_LC}::OpenGLES)
  list(APPEND SOURCES WinSystemWasmGLESContext.cpp)
  list(APPEND HEADERS WinSystemWasmGLESContext.h)
endif()

core_add_library(windowing_wasm)
