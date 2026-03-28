set(SOURCES CPUInfoWasm.cpp
            EmuEnvironStub.cpp
            GPUInfoWasm.cpp
            MemUtils.cpp
            PlatformWasm.cpp)

set(HEADERS CPUInfoWasm.h
            GPUInfoWasm.h
            PlatformWasm.h)

core_add_library(platform_wasm)
