set(SOURCES CPUInfoWasm.cpp
            GPUInfoWasm.cpp
            MemUtils.cpp
            PlatformWasm.cpp
            TizenWebApis.cpp
            WasmAudioWorkletManager.cpp)

set(HEADERS CPUInfoWasm.h
            GPUInfoWasm.h
            PlatformWasm.h
            TizenWebApis.h
            WasmMediaLayer.h
            WasmAudioWorkletManager.h)

core_add_library(platform_wasm)
