/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "WasmFilesystem.h"

#include "utils/log.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// clang-format off (JavaScript: EM_JS; uses === / !==)
EM_JS(void, wasm_fs_init, (), {
  try
  {
    if (typeof FS === 'undefined')
      return;
    if (!FS.analyzePath('/kodi_profile').exists)
      FS.mkdir('/kodi_profile');
    try
    {
      FS.mount(IDBFS, {}, '/kodi_profile');
    }
    catch (e)
    {
      console.warn('IDBFS mount:', e);
    }
    FS.syncfs(
        true, function(err) {
          if (err)
            console.warn('IDBFS syncfs:', err);
        });
  }
  catch (e)
  {
    console.warn('wasm_fs_init:', e);
  }
});
// clang-format on
#endif

namespace KODI::PLATFORM::WASM
{
void EnsureVirtualFilesystem()
{
#ifdef __EMSCRIPTEN__
  wasm_fs_init();
  CLog::Log(LOGINFO, "WASM: virtual filesystem (MEMFS/IDBFS) initialized");
#else
  CLog::Log(LOGDEBUG, "WASM filesystem stub: not Emscripten");
#endif
}
} // namespace KODI::PLATFORM::WASM
