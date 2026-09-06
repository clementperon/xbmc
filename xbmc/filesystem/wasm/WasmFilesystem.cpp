/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "WasmFilesystem.h"

#include "utils/log.h"

namespace KODI::PLATFORM::WASM
{
void EnsureVirtualFilesystem()
{
  CLog::Log(LOGINFO, "WASM: user profile persistence handled by kodi_pre.js (IDBFS)");
}
} // namespace KODI::PLATFORM::WASM
