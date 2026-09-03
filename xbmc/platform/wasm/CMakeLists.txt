set(SOURCES CPUInfoWasm.cpp
            DebugLog.cpp
            GPUInfoWasm.cpp
            MemUtils.cpp
            PlatformWasm.cpp
            TizenWebApis.cpp
            WasmAudioWorkletManager.cpp)

set(HEADERS CPUInfoWasm.h
            DebugLog.h
            GPUInfoWasm.h
            PlatformWasm.h
            TizenWebApis.h
            WasmAudioWorkletManager.h)

core_add_library(platform_wasm)
