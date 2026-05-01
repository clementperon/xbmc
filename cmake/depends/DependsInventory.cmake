# Legacy tools/depends package inventory mirrored into CMake.

# Native packages from tools/depends/native/Makefile
set(KODI_DEPENDS_NATIVE_BASE
    autoconf
    autoconf-archive
    automake
    cmake
    bison
    gas-preprocessor
    gettext
    giflib
    heimdal
    libjpeg-turbo
    liblzo2
    libpng
    libtool
    m4
    meson
    nasm
    ninja
    openssl
    pcre2
    perlmodule-parseyapp
    pythonmodule-pybind11
    pythonmodule-setuptools
    pkg-config
    python3
    swig
    TexturePacker
    zlib
)

set(KODI_DEPENDS_NATIVE_NOT_OSX libffi)
set(KODI_DEPENDS_NATIVE_DARWIN_EMBEDDED dpkg xz tar ldid)
set(KODI_DEPENDS_NATIVE_LINUX expat wayland-scanner pugixml waylandpp-scanner)
set(KODI_DEPENDS_NATIVE_LINUX_GLES MarkupSafe Mako)
set(KODI_DEPENDS_NATIVE_LINUX_WEBOS rustup cargo-c)
set(KODI_DEPENDS_NATIVE_ANDROID rustup cargo-c)

# Target packages from tools/depends/target/Makefile
set(KODI_DEPENDS_TARGET_BASE
    bzip2
    expat
    fontconfig
    freetype2
    freetype2-noharfbuzz
    fribidi
    fstrcmp
    gettext
    gmp
    gnutls
    gtest
    harfbuzz
    libffi
    libgcrypt
    libgpg-error
    libjpeg-turbo
    libplist
    libpng
    libshairplay
    libusb
    libxml2
    nettle
    openssl
    python3
    pythonmodule-pil
    pythonmodule-pycryptodome
    pythonmodule-setuptools
    sqlite3
    tinyxml
    xz
)

set(KODI_DEPENDS_TARGET_GPLV3 samba-gplv3 libcdio-gplv3)
set(KODI_DEPENDS_TARGET_NON_GPLV3 samba libcdio)

set(KODI_DEPENDS_TARGET_DARWIN_EMBEDDED_ADD darwin-embedded-entitlements)
set(KODI_DEPENDS_TARGET_DARWIN_EMBEDDED_EXCLUDE libusb gtest libcdio libcdio-gplv3)
set(KODI_DEPENDS_TARGET_TVOS_EXTRA_EXCLUDE libshairplay libplist)

set(KODI_DEPENDS_TARGET_OSX_ADD smctemp libaacs libbdplus apache-ant)
set(KODI_DEPENDS_TARGET_OSX_EXCLUDE libusb)

set(KODI_DEPENDS_TARGET_ANDROID_ADD dummy-libxbmc libdovi libuuid)
set(KODI_DEPENDS_TARGET_ANDROID_EXCLUDE libusb gtest libcdio libcdio-gplv3)

set(KODI_DEPENDS_TARGET_WASM_ADD libuuid)
set(KODI_DEPENDS_TARGET_WASM_EXCLUDE libusb samba samba-gplv3 libcdio libcdio-gplv3)

set(KODI_DEPENDS_TARGET_ZLIB zlib)
set(KODI_DEPENDS_TARGET_ICONV libiconv)

set(KODI_DEPENDS_TARGET_LINUX_ADD
    dbus
    libuuid
    alsa-lib
    libdrm
    libxkbcommon
    libinput
    libudev
    libevdev
    mtdev
    pipewire
    dav1d
    ffmpeg
)

set(KODI_DEPENDS_TARGET_LINUX_X86_64_ADD libva)
set(KODI_DEPENDS_TARGET_LINUX_WAYLAND_ADD wayland waylandpp wayland-protocols)
set(KODI_DEPENDS_TARGET_LINUX_X11_ADD linux-system-x11-libs)
set(KODI_DEPENDS_TARGET_LINUX_GL_ADD linux-system-gl-libs)
set(KODI_DEPENDS_TARGET_LINUX_GLES_ADD mesa)
set(KODI_DEPENDS_TARGET_WEBOS_ADD libdovi wayland waylandpp wayland-protocols webos-wayland-extensions webos-userland)
set(KODI_DEPENDS_TARGET_WEBOS_EXCLUDE dbus mtdev libevdev libinput linux-system-x11-libs pipewire mesa)
set(KODI_DEPENDS_TARGET_GBM_ADD hwdata libdisplay-info)

# Dependency edges mirrored from native/target Makefiles.
# Format: "<pkg>:<dep1>;<dep2>"
set(KODI_DEPENDS_NATIVE_DEPENDENCY_PAIRS
    "autoconf-archive:autoconf"
    "autoconf:m4"
    "automake:autoconf"
    "bison:gettext"
    "cargo-c:pkg-config;openssl;rustup"
    "dpkg:automake;gettext;libtool;pkg-config;tar"
    "heimdal:libtool"
    "JsonSchemaBuilder:cmake"
    "libjpeg-turbo:cmake;nasm"
    "libpng:zlib;automake"
    "libtool:automake"
    "Mako:MarkupSafe"
    "meson:python3;pythonmodule-setuptools"
    "ninja:meson"
    "openssl:zlib"
    "pcre2:cmake"
    "pkgconf:meson;ninja"
    "pugixml:cmake"
    "python3:expat;libffi;pkg-config;zlib;openssl;autoconf-archive"
    "pythonmodule-pybind11:pythonmodule-setuptools;python3"
    "pythonmodule-setuptools:python3"
    "swig:bison;cmake;pcre2"
    "tar:xz;automake"
    "TexturePacker:cmake;libpng;liblzo2;giflib;libjpeg-turbo"
    "wayland-scanner:expat;ninja;pkg-config"
    "waylandpp-scanner:cmake;pugixml"
    "MarkupSafe:ninja"
    "liblzo2:automake"
)

set(KODI_DEPENDS_TARGET_DEPENDENCY_PAIRS
    "cec:p8-platform"
    "crossguid:libuuid"
    "curl:brotli;openssl;nghttp2;zlib"
    "dbus:expat"
    "exiv2:libiconv;zlib;expat"
    "ffmpeg:libiconv;zlib;bzip2;gnutls;dav1d;libva"
    "fontconfig:freetype2;expat;libiconv;libuuid"
    "freetype2:bzip2;harfbuzz;zlib"
    "gettext:libiconv"
    "gnutls:nettle;zlib"
    "harfbuzz:freetype2-noharfbuzz;libiconv"
    "libaacs:libgcrypt;libgpg-error"
    "libass:fontconfig;fribidi;harfbuzz;freetype2;libiconv"
    "libbdplus:libaacs;libgcrypt;libgpg-error"
    "libbluray:fontconfig;freetype2;libiconv;udfread;libxml2;libaacs;libbdplus;apache-ant"
    "libcdio-gplv3:libiconv"
    "libcdio:libiconv"
    "libdisplay-info:hwdata"
    "libevdev:libudev"
    "libgcrypt:libgpg-error"
    "libinput:mtdev;libevdev"
    "libmicrohttpd:gnutls;libgcrypt;libgpg-error"
    "libplist:zlib"
    "libpng:zlib"
    "libva:libdrm;wayland;linux-system-x11-libs"
    "libxml2:zlib"
    "libxslt:libxml2"
    "libzip:bzip2;gnutls;zlib"
    "mariadb:openssl;libiconv;zlib"
    "mesa:libdrm;wayland-protocols;wayland;linux-system-x11-libs;zlib"
    "nettle:gmp"
    "openssl:zlib"
    "python3:expat;gettext;libxml2;sqlite3;openssl;libffi;bzip2;xz;libiconv"
    "pythonmodule-pil:bzip2;dummy-libxbmc;zlib;libjpeg-turbo;libpng;freetype2;python3;pythonmodule-setuptools"
    "pythonmodule-pycryptodome:dummy-libxbmc;python3;pythonmodule-setuptools"
    "pythonmodule-setuptools:dummy-libxbmc;python3"
    "samba-gplv3:gnutls;zlib"
    "taglib:utf8-cpp;zlib"
    "wayland:expat;libffi"
    "waylandpp:wayland;linux-system-gl-libs;mesa"
    "xz:gettext"
)
