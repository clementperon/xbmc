/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "WinEventsWasm.h"
#include "rendering/gles/RenderSystemGLES.h"
#include "windowing/WinSystem.h"

#include <emscripten/html5.h>

#include <memory>

class CWinSystemWasmGLESContext : public CWinSystemBase, public CRenderSystemGLES
{
public:
  CWinSystemWasmGLESContext();
  ~CWinSystemWasmGLESContext() override;

  static void Register();
  static std::unique_ptr<CWinSystemBase> CreateWinSystem();

  CRenderSystemBase* GetRenderSystem() override { return this; }

  bool InitWindowSystem() override;
  bool DestroyWindowSystem() override;

  bool CreateNewWindow(const std::string& name, bool fullScreen, RESOLUTION_INFO& res) override;
  bool DestroyWindow() override;
  void UpdateResolutions() override;

  bool ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop) override;
  bool SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays) override;

  bool MessagePump() override;

  void Register(IDispResource* resource) override {}
  void Unregister(IDispResource* resource) override {}

protected:
  void SetVSyncImpl(bool enable) override;
  void PresentRenderImpl(bool rendered) override;

private:
  EMSCRIPTEN_WEBGL_CONTEXT_HANDLE m_webglContext{0};
};
