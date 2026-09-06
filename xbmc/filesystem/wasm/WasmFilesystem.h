/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

namespace KODI::PLATFORM::WASM
{
/*! \brief Mount MEMFS/IDBFS paths used by special:// URLs in the browser. */
void EnsureVirtualFilesystem();
} // namespace KODI::PLATFORM::WASM
