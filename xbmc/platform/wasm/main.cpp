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
#include <sys/stat.h>
#include <unistd.h>

#include <emscripten/heap.h>
#include <emscripten/threading.h>

int main(int argc, char* argv[])
{
  auto logVfsProbe = [](const char* path, bool isDir) {
    struct stat st = {};
    const int rc = stat(path, &st);
    const bool exists = (rc == 0);
    const bool typeOk = exists && (isDir ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode));
    std::fprintf(stderr, "WASM: VFS probe path=%s exists=%d typeOk=%d\n", path, exists ? 1 : 0,
                 typeOk ? 1 : 0);
  };

  setlocale(LC_NUMERIC, "C");

  // Emscripten VFS: data is preloaded under /kodi via --preload-file
  setenv("KODI_HOME", "/kodi", 0);
  std::fprintf(stderr, "WASM: KODI_HOME=%s\n", getenv("KODI_HOME"));
  logVfsProbe("/kodi/system", true);
  logVfsProbe("/kodi/addons", true);
  logVfsProbe("/kodi/media", true);
  logVfsProbe("/kodi/userdata", true);
  logVfsProbe("/kodi/addons/skin.estuary/addon.xml", false);

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
