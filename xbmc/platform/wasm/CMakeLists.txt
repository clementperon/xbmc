set(SOURCES CPUInfoWasm.cpp
            GPUInfoWasm.cpp
            MemUtils.cpp
            PlatformWasm.cpp
            TizenWebApis.cpp
            WasmAudioWorkletManager.cpp)

set(HEADERS CPUInfoWasm.h
            GPUInfoWasm.h
            MseBridge.h
            PlatformWasm.h
            TizenEmss.h
            TizenWebApis.h
            WasmMediaLayer.h
            WasmAudioWorkletManager.h)

core_add_library(platform_wasm)
