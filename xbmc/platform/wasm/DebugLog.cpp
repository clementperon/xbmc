/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// #region agent log: temporary debug instrumentation, remove when done.
#include "DebugLog.h"

#include <emscripten/em_asm.h>

namespace KODI::PLATFORM::WASM::DEBUGLOG
{

// Logs to console.log with a "[KODI_DBG]" prefix so the user can grep it
// out of the regular kodi.js console output.  Network logging can't be
// used because the runtime target (Tizen TV) cannot reach the dev host.
void Post(const char* location, const char* message, const std::string& dataJson)
{
  EM_ASM(
      {
        try
        {
          const loc = UTF8ToString($0);
          const msg = UTF8ToString($1);
          const dataStr = UTF8ToString($2);
          console.log('[KODI_DBG] ' + loc + ' | ' + msg + ' | ' + dataStr);
        }
        catch (e) {}
      },
      location, message, dataJson.c_str());
}

double NowMs()
{
  return EM_ASM_DOUBLE({ return Date.now(); });
}

void InstallMainHeartbeat()
{
}

double MainHeartbeatLastMs()
{
  // Heartbeat now logs to console; this accessor is no longer used.
  return 0.0;
}

} // namespace KODI::PLATFORM::WASM::DEBUGLOG
// #endregion
