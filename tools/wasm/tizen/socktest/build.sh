#!/usr/bin/env sh
# Build, package and optionally install the Tizen sockets test app.
#
#   tools/wasm/tizen/socktest/build.sh [--install] [--run]
#
# Environment:
#   EMCC         emcc to use (default: emcc from PATH)
#   DEPENDS      Kodi depends prefix with the wasm libcurl and OpenSSL
#                (default: /opt/xbmc-deps/wasm32-unknown-emscripten-release)
#   TZ           Tizen tz CLI (default: ~/tizen-studio/tools/tizen-core/tz)
#   SDB          sdb (default: ~/tizen-studio/tools/sdb)
#   TIZEN_TARGET_SERIAL  device for tz install / sdb (optional)
#   OUT          build directory (default: build-wasm/socktest under the repo)
set -eu

HERE=$(cd "$(dirname "$0")" && pwd -P)
ROOT=$(cd "$HERE/../../../.." && pwd -P)
EMCC=${EMCC:-emcc}
DEPENDS=${DEPENDS:-/opt/xbmc-deps/wasm32-unknown-emscripten-release}
TZ=${TZ:-$HOME/tizen-studio/tools/tizen-core/tz}
SDB=${SDB:-$HOME/tizen-studio/tools/sdb}
OUT=${OUT:-$ROOT/build-wasm/socktest}
APP_ID=kodisocket.SockTest
PKG_ID=kodisocket

mkdir -p "$OUT"
cp "$HERE/index.html" "$HERE/config.xml" "$HERE/tizen_web_project.yaml" "$OUT/"
cp "$ROOT/tools/wasm/tizen/icon.png" "$OUT/icon.png"
cp "$ROOT/system/certs/cacert.pem" "$OUT/cacert.pem"

echo "Compiling socktest..."
"$EMCC" -O1 -g1 \
  -pthread -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=4 \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB \
  -sASSERTIONS=1 -sEXIT_RUNTIME=0 \
  -sENVIRONMENT=web,worker \
  --js-library "$ROOT/xbmc/platform/wasm/network/tizen_sockets.js" \
  -I"$ROOT/xbmc" -I"$DEPENDS/include" -DCURL_STATICLIB \
  "$ROOT/xbmc/platform/wasm/network/TizenSockets.c" \
  "$HERE/main.c" \
  -L"$DEPENDS/lib" -lcurl -lssl -lcrypto -lz -lbrotlidec -lbrotlicommon -lnghttp2 \
  --preload-file "$OUT/cacert.pem@/cacert.pem" \
  -o "$OUT/socktest.js"
echo "Built $OUT/socktest.js"

if [ "${1:-}" = "--install" ] || [ "${2:-}" = "--install" ]; then
  rm -rf "$OUT/Debug"
  "$TZ" pack -w "$OUT" -t wgt
  set --
  if [ -n "${TIZEN_TARGET_SERIAL:-}" ]; then
    set -- --serial "$TIZEN_TARGET_SERIAL"
  fi
  "$TZ" install --package-path "$(ls "$OUT"/Debug/*.wgt | head -n 1)" "$@"
fi

if [ "${1:-}" = "--run" ] || [ "${2:-}" = "--run" ]; then
  set --
  if [ -n "${TIZEN_TARGET_SERIAL:-}" ]; then
    set -- -s "$TIZEN_TARGET_SERIAL"
  fi
  "$SDB" "$@" shell 0 kill "$PKG_ID" >/dev/null 2>&1 || true
  "$SDB" "$@" shell 0 debug "$APP_ID"
fi
