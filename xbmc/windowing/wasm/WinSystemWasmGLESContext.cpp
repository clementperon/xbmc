/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "WinSystemWasmGLESContext.h"

#include "WasmClipboard.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecWebCodecs.h"
#include "cores/VideoPlayer/Process/wasm/ProcessInfoWasm.h"
#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGLES.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "rendering/gles/ScreenshotSurfaceGLES.h"
#include "settings/DisplaySettings.h"
#include "platform/wasm/DebugLog.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WindowSystemFactory.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

#include <emscripten/em_asm.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>

#include <emscripten/html5_webgl.h>

// See docs/wasm/RENDERING.md for the rationale behind this architecture.

namespace
{
// Incremented by the main thread's requestAnimationFrame pump; PresentRenderImpl
// waits on it so the Kodi thread never renders faster than the display.
volatile uint32_t g_vsyncTick = 0;
bool g_vsyncPumpInstalled = false;
constexpr double VSYNC_WAIT_TIMEOUT_MS = 100.0;

void InstallVsyncPump()
{
  if (g_vsyncPumpInstalled)
    return;
  g_vsyncPumpInstalled = true;

  MAIN_THREAD_EM_ASM(
      {
        const idx = $0 >> 2;
        let lastTimestamp = 0.0;
        let measuredRefreshRate = 0.0;
        globalThis.__kodiRefreshRate = 60.0;
        const tick = (timestamp) =>
        {
          if (lastTimestamp > 0.0)
          {
            const intervalMs = timestamp - lastTimestamp;
            if (intervalMs > 0.0)
            {
              const hz = 1000.0 / intervalMs;
              if (hz >= 20.0 && hz <= 240.0)
              {
                measuredRefreshRate =
                  measuredRefreshRate > 0.0 ? measuredRefreshRate * 0.9 + hz * 0.1 : hz;
                globalThis.__kodiRefreshRate = measuredRefreshRate;
              }
            }
          }
          lastTimestamp = timestamp;
          Atomics.add(HEAP32, idx, 1);
          Atomics.notify(HEAP32, idx, 1);
          requestAnimationFrame(tick);
        };
        requestAnimationFrame(tick);
      },
      &g_vsyncTick);
}

bool IsTizenRuntime()
{
  return MAIN_THREAD_EM_ASM_INT(
             {
               const ua = globalThis.navigator && globalThis.navigator.userAgent
                            ? globalThis.navigator.userAgent
                            : "";
               return !!(globalThis.tizen || globalThis.webapis || /Tizen/i.test(ua));
             }) != 0;
}

// The context is created on the browser main thread and proxied to this pthread:
// a thread that never returns to its event loop (Kodi's modal dialog loops) must
// not own per-frame browser resources, see docs/wasm/RENDERING.md.
EMSCRIPTEN_WEBGL_CONTEXT_HANDLE CreateProxiedGLContext()
{
  EmscriptenWebGLContextAttributes attrs;
  emscripten_webgl_init_context_attributes(&attrs);
  attrs.alpha = EM_FALSE;
  attrs.depth = EM_TRUE;
  attrs.stencil = EM_TRUE;
  attrs.antialias = EM_FALSE;
  attrs.premultipliedAlpha = EM_TRUE;
  attrs.preserveDrawingBuffer = EM_FALSE;
  attrs.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
  attrs.failIfMajorPerformanceCaveat = EM_FALSE;
  attrs.majorVersion = 2;
  attrs.minorVersion = 0;
  attrs.enableExtensionsByDefault = EM_TRUE;
  attrs.explicitSwapControl = EM_TRUE;
  attrs.renderViaOffscreenBackBuffer = EM_TRUE;
  attrs.proxyContextToMainThread = EMSCRIPTEN_WEBGL_CONTEXT_PROXY_ALWAYS;
  return emscripten_webgl_create_context("#canvas", &attrs);
}

void NotifyFirstFramePresented()
{
  MAIN_THREAD_ASYNC_EM_ASM({
    if (typeof Module.onKodiFirstFramePresented === 'function')
      Module.onKodiFirstFramePresented();
  });
}

void GetCanvasSize(int* width, int* height)
{
  double w = 0;
  double h = 0;
  emscripten_get_element_css_size("#canvas", &w, &h);
  *width = static_cast<int>(w);
  *height = static_cast<int>(h);
  if (*width <= 0)
    *width = 1280;
  if (*height <= 0)
    *height = 720;
}

double GetBrowserRefreshRate()
{
  const double refreshRate = MAIN_THREAD_EM_ASM_DOUBLE(
      {
        const rate = globalThis.__kodiRefreshRate;
        return Number.isFinite(rate) && rate >= 20.0 && rate <= 240.0 ? rate : 60.0;
      });
  return refreshRate;
}
} // namespace

void CWinSystemWasmGLESContext::Register()
{
  KODI::WINDOWING::CWindowSystemFactory::RegisterWindowSystem(CreateWinSystem);
}

std::unique_ptr<CWinSystemBase> CWinSystemWasmGLESContext::CreateWinSystem()
{
  return std::make_unique<CWinSystemWasmGLESContext>();
}

CWinSystemWasmGLESContext::CWinSystemWasmGLESContext()
{
  m_winEvents = std::make_unique<CWinEventsWasm>();
}

CWinSystemWasmGLESContext::~CWinSystemWasmGLESContext()
{
  if (m_webglContext > 0)
    emscripten_webgl_destroy_context(m_webglContext);
}

bool CWinSystemWasmGLESContext::InitWindowSystem()
{
  VIDEOPLAYER::CRendererFactory::ClearRenderer();
  CProcessInfoWasm::Register();
  CDVDVideoCodecWebCodecs::Register();
  CLinuxRendererGLES::Register();
  CScreenshotSurfaceGLES::Register();

  int initW = 0;
  int initH = 0;
  GetCanvasSize(&initW, &initH);
  emscripten_set_canvas_element_size("#canvas", initW, initH);

  m_webglContext = CreateProxiedGLContext();
  if (m_webglContext <= 0)
  {
    CLog::Log(LOGERROR, "WASM: failed to create proxied WebGL2 context ({})", m_webglContext);
    return false;
  }

  const EMSCRIPTEN_RESULT r = emscripten_webgl_make_context_current(m_webglContext);
  if (r != EMSCRIPTEN_RESULT_SUCCESS)
  {
    CLog::Log(LOGERROR, "WASM: emscripten_webgl_make_context_current failed ({})", r);
    return false;
  }

  InstallVsyncPump();
  if (IsTizenRuntime())
    CLog::Log(LOGINFO, "WASM: Tizen runtime detected");

  CLog::Log(LOGINFO, "WASM: using main-thread WebGL2 context proxied to the Kodi thread ({}x{})",
            initW, initH);
  return CWinSystemBase::InitWindowSystem();
}

bool CWinSystemWasmGLESContext::DestroyWindowSystem()
{
  if (m_webglContext > 0)
  {
    emscripten_webgl_destroy_context(m_webglContext);
    m_webglContext = 0;
  }
  return CWinSystemBase::DestroyWindowSystem();
}

bool CWinSystemWasmGLESContext::CreateNewWindow(const std::string& name,
                                                bool fullScreen,
                                                RESOLUTION_INFO& res)
{
  // Render at the viewport's CSS pixel size to keep mouse input 1:1
  // (see docs/wasm/RENDERING.md §2.4).
  int w = 0;
  int h = 0;
  GetCanvasSize(&w, &h);

  UpdateDesktopResolution(res, "Browser", w, h, static_cast<float>(GetBrowserRefreshRate()), 0);
  res.bFullScreen = fullScreen;

  emscripten_set_canvas_element_size("#canvas", w, h);

  if (emscripten_webgl_get_current_context() != m_webglContext)
    emscripten_webgl_make_context_current(m_webglContext);

  m_nWidth = static_cast<unsigned int>(w);
  m_nHeight = static_cast<unsigned int>(h);
  m_bWindowCreated = true;

  CDisplaySettings::GetInstance().GetResolutionInfo(RES_DESKTOP) = res;
  SetWindowResolution(w, h);
  CRenderSystemGLES::ResetRenderSystem(w, h);

  CLog::Log(LOGINFO, "WASM: created GLES window {}x{} ({})", w, h, name);
  return true;
}

bool CWinSystemWasmGLESContext::DestroyWindow()
{
  m_bWindowCreated = false;
  return true;
}

void CWinSystemWasmGLESContext::UpdateResolutions()
{
  CWinSystemBase::UpdateResolutions();
}

bool CWinSystemWasmGLESContext::ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop)
{
  emscripten_set_canvas_element_size("#canvas", newWidth, newHeight);
  SetWindowResolution(newWidth, newHeight);
  CRenderSystemGLES::ResetRenderSystem(newWidth, newHeight);
  m_nWidth = static_cast<unsigned int>(newWidth);
  m_nHeight = static_cast<unsigned int>(newHeight);
  return true;
}

bool CWinSystemWasmGLESContext::SetFullScreen(bool fullScreen,
                                              RESOLUTION_INFO& res,
                                              bool blankOtherDisplays)
{
  return CreateNewWindow("", fullScreen, res);
}

void CWinSystemWasmGLESContext::ForceFullScreen(const RESOLUTION_INFO& resInfo)
{
  // AppInboundProtocol calls us with the cached RES_DESKTOP on browser resize,
  // so resInfo is stale.  Re-query the live canvas CSS size and update
  // RES_DESKTOP + the cached mouse resolution so mouse events (which arrive in
  // CSS pixels) stay identity-mapped to Kodi's GUI coordinate space.
  int w = 0;
  int h = 0;
  GetCanvasSize(&w, &h);
  if (w <= 0 || h <= 0)
    return;

  RESOLUTION_INFO& desktop = CDisplaySettings::GetInstance().GetResolutionInfo(RES_DESKTOP);
  const std::string output = desktop.strOutput.empty() ? "Browser" : desktop.strOutput;
  const uint32_t flags = desktop.dwFlags;
  UpdateDesktopResolution(desktop, output, w, h, static_cast<float>(GetBrowserRefreshRate()),
                          flags);
  GetGfxContext().ResetOverscan(desktop);

  ResizeWindow(w, h, 0, 0);

  GetGfxContext().ApplyModeChange(RES_DESKTOP);
}

bool CWinSystemWasmGLESContext::MessagePump()
{
  if (m_winEvents)
    return m_winEvents->MessagePump();
  return false;
}

void CWinSystemWasmGLESContext::SetVSyncImpl(bool enable)
{
}

void CWinSystemWasmGLESContext::PresentRenderImpl(bool rendered)
{
  if (!rendered || m_webglContext <= 0)
    return;

  // #region agent log: temporary present-path timing, remove when done.
  static uint32_t s_frames = 0;
  static uint32_t s_timeouts = 0;
  static double s_waitMs = 0.0;
  static double s_commitMs = 0.0;
  static double s_commitMaxMs = 0.0;
  static double s_lastReportMs = 0.0;
  const double t0 = emscripten_get_now();
  // #endregion

  // The timeout keeps Kodi running when rAF is throttled (hidden tab).
  const uint32_t current = __atomic_load_n(&g_vsyncTick, __ATOMIC_ACQUIRE);
  if (current == m_lastVsyncSeen)
    emscripten_futex_wait(const_cast<uint32_t*>(&g_vsyncTick), current, VSYNC_WAIT_TIMEOUT_MS);

  const uint32_t next = __atomic_load_n(&g_vsyncTick, __ATOMIC_ACQUIRE);
  // #region agent log
  const double t1 = emscripten_get_now();
  s_waitMs += t1 - t0;
  // #endregion
  if (next == m_lastVsyncSeen)
  {
    // #region agent log
    ++s_timeouts;
    // #endregion
    return;
  }
  m_lastVsyncSeen = next;

  // Synchronous: blits the offscreen framebuffer to the canvas on the main
  // thread once every queued GL call before it has executed.
  const EMSCRIPTEN_RESULT r = emscripten_webgl_commit_frame();
  // #region agent log
  const double t2 = emscripten_get_now();
  s_commitMs += t2 - t1;
  if (t2 - t1 > s_commitMaxMs)
    s_commitMaxMs = t2 - t1;
  ++s_frames;
  if (t2 - s_lastReportMs >= 1000.0)
  {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "{\"frames\":%u,\"timeouts\":%u,\"avgWaitMs\":%.1f,\"avgCommitMs\":%.1f,"
                  "\"maxCommitMs\":%.1f,\"tick\":%u}",
                  s_frames, s_timeouts, s_waitMs / std::max(1u, s_frames + s_timeouts),
                  s_commitMs / std::max(1u, s_frames), s_commitMaxMs, next);
    KODI::PLATFORM::WASM::DEBUGLOG::Post("WinSystemWasmGLESContext.cpp:PresentRenderImpl",
                                         "present stats", buf);
    s_frames = s_timeouts = 0;
    s_waitMs = s_commitMs = s_commitMaxMs = 0.0;
    s_lastReportMs = t2;
  }
  // #endregion
  if (r != EMSCRIPTEN_RESULT_SUCCESS)
  {
    CLog::Log(LOGWARNING, "WASM: emscripten_webgl_commit_frame failed ({})", r);
    return;
  }
  if (!m_firstFramePresented)
  {
    m_firstFramePresented = true;
    NotifyFirstFramePresented();
  }
}

int CWinSystemWasmGLESContext::GetBufferAge()
{
  return 1;
}

std::string CWinSystemWasmGLESContext::GetClipboardText()
{
  return WASM_CLIPBOARD::ConsumePendingPasteText();
}
