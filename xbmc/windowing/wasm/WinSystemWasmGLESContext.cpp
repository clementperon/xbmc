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
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WindowSystemFactory.h"

#include <cstdint>
#include <string>

#include <emscripten/em_asm.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>

#include <GLES2/gl2.h>

// See docs/wasm/RENDERING.md for the rationale behind this architecture.

namespace
{
// Shared-memory atomic incremented by the main thread's requestAnimationFrame
// pump (InstallVsyncPump).  Presenters wait on it via emscripten_futex_wait
// to pace to the browser compositor.
volatile uint32_t g_vsyncTick = 0;
bool g_vsyncPumpInstalled = false;

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

// Creates a standalone OffscreenCanvas + WebGL2 context on the current
// pthread and registers it with Emscripten's GL layer.  Returns 0 on failure.
EMSCRIPTEN_WEBGL_CONTEXT_HANDLE CreateWorkerGLContext(int width, int height)
{
  const EMSCRIPTEN_WEBGL_CONTEXT_HANDLE handle = EM_ASM_INT(
      {
        try
        {
          const w = $0 > 0 ? $0 : 1280;
          const h = $1 > 0 ? $1 : 720;
          if (typeof OffscreenCanvas === 'undefined') {
            console.error('[kodi] OffscreenCanvas not available on worker.');
            return 0;
          }
          const off = new OffscreenCanvas(w, h);
          // Fields assigned one-per-line: the C preprocessor would split
          // EM_ASM_INT's arguments on any top-level comma inside a literal.
          const attrs = {};
          attrs.alpha = false;
          attrs.depth = true;
          attrs.stencil = true;
          attrs.antialias = false;
          attrs.premultipliedAlpha = true;
          attrs.preserveDrawingBuffer = false;
          attrs.powerPreference = 'high-performance';
          attrs.failIfMajorPerformanceCaveat = false;
          const ctx = off.getContext('webgl2', attrs);
          if (!ctx) {
            console.error('[kodi] getContext("webgl2") returned null.');
            return 0;
          }
          globalThis.__kodiOffCanvas = off;
          globalThis.__kodiGL = ctx;
          const ctxAttrs = {};
          ctxAttrs.majorVersion = 2;
          ctxAttrs.minorVersion = 0;
          ctxAttrs.alpha = attrs.alpha;
          ctxAttrs.depth = attrs.depth;
          ctxAttrs.stencil = attrs.stencil;
          ctxAttrs.antialias = attrs.antialias;
          ctxAttrs.premultipliedAlpha = attrs.premultipliedAlpha;
          ctxAttrs.preserveDrawingBuffer = attrs.preserveDrawingBuffer;
          ctxAttrs.powerPreference = attrs.powerPreference;
          ctxAttrs.failIfMajorPerformanceCaveat = attrs.failIfMajorPerformanceCaveat;
          ctxAttrs.enableExtensionsByDefault = true;
          ctxAttrs.explicitSwapControl = false;
          ctxAttrs.renderViaOffscreenBackBuffer = false;
          ctxAttrs.proxyContextToMainThread = 0;
          return GL.registerContext(ctx, ctxAttrs);
        }
        catch (e)
        {
          console.error('[kodi] CreateWorkerGLContext failed:', e);
          return 0;
        }
      },
      width, height);
  return handle;
}

// Resizes the OffscreenCanvas's drawing buffer.
void ResizeWorkerGLContext(int width, int height)
{
  EM_ASM(
      {
        const off = globalThis.__kodiOffCanvas;
        if (off)
        {
          off.width = $0;
          off.height = $1;
        }
      },
      width, height);
}

// Zero-copy frame handoff to the main thread.  Emscripten's main-thread
// onmessage handler dispatches cmd:'callHandler' to Module[handler](...args).
void PostFrameBitmap()
{
  EM_ASM({
    try
    {
      const off = globalThis.__kodiOffCanvas;
      if (!off)
        return;
      const bm = off.transferToImageBitmap();
      const msg = {};
      msg.cmd = 'callHandler';
      msg.handler = 'onKodiFrame';
      msg.args = [ bm ];
      postMessage(msg, [ bm ]);
    }
    catch (e)
    {
      console.error('[kodi] transferToImageBitmap/postMessage failed:', e);
    }
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

  m_webglContext = CreateWorkerGLContext(initW, initH);
  if (m_webglContext <= 0)
  {
    CLog::Log(LOGERROR, "WASM: failed to create standalone OffscreenCanvas WebGL2 context");
    return false;
  }

  const EMSCRIPTEN_RESULT r = emscripten_webgl_make_context_current(m_webglContext);
  if (r != EMSCRIPTEN_RESULT_SUCCESS)
  {
    CLog::Log(LOGERROR, "WASM: emscripten_webgl_make_context_current failed ({})", r);
    return false;
  }

  InstallVsyncPump();

  CLog::Log(LOGINFO, "WASM: using standalone OffscreenCanvas + transferToImageBitmap ({}x{})",
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

  ResizeWorkerGLContext(w, h);

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
  ResizeWorkerGLContext(newWidth, newHeight);
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

  if (emscripten_webgl_get_current_context() != m_webglContext)
  {
    const EMSCRIPTEN_RESULT makeCurrent = emscripten_webgl_make_context_current(m_webglContext);
    if (makeCurrent != EMSCRIPTEN_RESULT_SUCCESS)
    {
      CLog::Log(LOGWARNING, "WASM: PresentRenderImpl failed make_context_current ({})",
                makeCurrent);
      return;
    }
  }

  // Flush so transferToImageBitmap() below sees a fully-rendered frame.
  glFlush();

  PostFrameBitmap();

  // Pace to main-thread rAF (see docs/wasm/RENDERING.md §2.3); 100 ms
  // timeout keeps Kodi running when rAF is throttled.
  const uint32_t current = __atomic_load_n(&g_vsyncTick, __ATOMIC_ACQUIRE);
  if (current == m_lastVsyncSeen)
    emscripten_futex_wait(const_cast<uint32_t*>(&g_vsyncTick), current, 100.0);
  m_lastVsyncSeen = __atomic_load_n(&g_vsyncTick, __ATOMIC_ACQUIRE);
}

int CWinSystemWasmGLESContext::GetBufferAge()
{
  return 0;
}

std::string CWinSystemWasmGLESContext::GetClipboardText()
{
  return WASM_CLIPBOARD::ConsumePendingPasteText();
}
