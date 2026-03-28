/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "application/AppEnvironment.h"
#include "application/AppParamParser.h"
#include "platform/xbmc.h"

#include <cstdio>
#include <locale.h>
#include <new>
#include <stdlib.h>

#include <emscripten/heap.h>
#include <emscripten/threading.h>

int main(int argc, char* argv[])
{
  setlocale(LC_NUMERIC, "C");

  // Emscripten VFS: data is preloaded under /kodi via --preload-file
  setenv("KODI_HOME", "/kodi", 0);

  CAppParamParser appParamParser;
  appParamParser.Parse(argv, argc);

  CAppEnvironment::SetUp(appParamParser.GetAppParams());
#ifndef TARGET_WASM
  const int status = XBMC_Run(true);
  CAppEnvironment::TearDown();
  return status;
#else
  (void)XBMC_Run(true);
  return 0;
#endif
}
