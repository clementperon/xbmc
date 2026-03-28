# Emscripten WebAssembly platform — GLES maps to WebGL2
set(PLATFORM_REQUIRED_DEPS OpenGLES EGL)
set(PLATFORM_OPTIONAL_DEPS_EXCLUDE
    Alsa
    Avahi
    Bluetooth
    Bluray
    CAP
    CEC
    DBus
    Iso9660pp
    LCMS2
    LircClient
    MDNS
    MicroHttpd
    NFS
    Pipewire
    Plist
    PulseAudio
    Python
    SmbClient
    Sndio
    UDEV
    Udfread
    XSLT)

if(NOT APP_RENDER_SYSTEM STREQUAL "gles")
  message(FATAL_ERROR "WASM build requires APP_RENDER_SYSTEM=gles (maps to WebGL2)")
endif()

list(APPEND GL_INTERFACES_LIST "GLES")
