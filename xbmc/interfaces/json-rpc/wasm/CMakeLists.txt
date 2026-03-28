if(NOT CORE_SYSTEM_NAME STREQUAL wasm)
  return()
endif()

set(SOURCES JSONRPCWasmBridge.cpp)
set(HEADERS)
core_add_library(interfaces_jsonrpc_wasm)
