/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Embind / postMessage bridge for JSON-RPC from host JavaScript (roadmap Phase 8).
 */

#ifdef TARGET_WASM
#include <emscripten.h>

extern "C" EMSCRIPTEN_KEEPALIVE void kodi_wasm_jsonrpc_notify_ready()
{
  EM_ASM({
    if (typeof Module !== 'undefined' && Module.onKodiReady)
      Module.onKodiReady();
    if (typeof window !== 'undefined' && window.dispatchEvent) {
      window.dispatchEvent(new CustomEvent('kodi-ready'));
    }
  });
}
#endif
