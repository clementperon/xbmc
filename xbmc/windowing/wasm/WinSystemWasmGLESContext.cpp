/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "WinSystemWasmGLESContext.h"

#include "cores/VideoPlayer/Process/wasm/ProcessInfoWasm.h"
#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGLES.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "rendering/gles/ScreenshotSurfaceGLES.h"
#include "settings/DisplaySettings.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WindowSystemFactory.h"

#include <emscripten.h>
#include <emscripten/html5.h>

namespace
{
// Run Emscripten's GL.newRenderingFrameStarted() temp-VBO swap on the *browser*
// main thread for the real WebGL context object (see Emscripten libwebgl.js).
// When WebGL is proxied from a pthread, registerPreMainLoop still calls
// newRenderingFrameStarted on the worker where GL.currentContext may be a stub
// without tempVertexBufferCounters1 — or Emscripten's original would crash.
// A full no-op breaks client-side vertex uploads (Kodi GUI), yielding a black
// offscreen buffer. Export is listed in cmake/scripts/wasm/ArchSetup.cmake.
extern "C" EMSCRIPTEN_KEEPALIVE void kodi_nfs_run_swap(int handle)
{
  MAIN_THREAD_EM_ASM(
  {
    var ctx = GL.contexts[$0];
    if (!ctx || !ctx.tempVertexBufferCounters1) return;
    var vb = ctx.tempVertexBuffers1;
    ctx.tempVertexBuffers1 = ctx.tempVertexBuffers2;
    ctx.tempVertexBuffers2 = vb;
    vb = ctx.tempVertexBufferCounters1;
    ctx.tempVertexBufferCounters1 = ctx.tempVertexBufferCounters2;
    ctx.tempVertexBufferCounters2 = vb;
    var largestIndex = GL.log2ceilLookup(GL.MAX_TEMP_BUFFER_SIZE);
    for (var i = 0; i <= largestIndex; ++i) {
      ctx.tempVertexBufferCounters1[i] = 0;
    }
  },
  handle);
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
  CLinuxRendererGLES::Register();
  CScreenshotSurfaceGLES::Register();

  EmscriptenWebGLContextAttributes attrs;
  emscripten_webgl_init_context_attributes(&attrs);
  attrs.majorVersion = 2;
  attrs.minorVersion = 0;
  attrs.alpha = false;
  attrs.depth = true;
  attrs.stencil = true;
  attrs.antialias = false;
  attrs.enableExtensionsByDefault = true;
  attrs.explicitSwapControl = true;
  // main() runs on a pthread (PROXY_TO_PTHREAD). PROXY_FALLBACK creates the
  // context on the main thread when called from a worker. OFFSCREEN_FRAMEBUFFER
  // (linker flag) provides the GL-call proxy infrastructure so all GL calls
  // from this pthread are serialised and executed on the main thread.
  //
  // renderViaOffscreenBackBuffer = true is required for correct frame presentation.
  // With PROXY_TO_PTHREAD the DOM canvas is unreachable from the worker, so
  // PROXY_FALLBACK proxies the context to the main thread. On modern browsers
  // _emscripten_supports_offscreencanvas() returns true, so Emscripten does NOT
  // automatically force renderViaOffscreenBackBuffer (libhtml5_webgl.js lines 120-137).
  // Without it GL.currentContext.defaultFbo is null, emscripten_webgl_commit_frame
  // becomes a no-op (emscripten_webgl_do_commit_frame checks defaultFbo first,
  // webgl1.c:114-119), and no blit ever reaches the canvas — black screen.
  // With renderViaOffscreenBackBuffer=true Emscripten creates an offscreen FBO;
  // commit_frame blits it to the canvas via GL.blitOffscreenFramebuffer.
  // kodi.html upgrades the FBO depth buffer to DEPTH24_STENCIL8 (wasmStencilHack,
  // on by default) so that GUI stencil clipping works.
  //
  // Note: GL_TEXTURE_SWIZZLE_R/G/B/A calls are disabled for TARGET_WASM in
  // TextureGLES.cpp.  The WebGL 2.0 spec explicitly removes TEXTURE_SWIZZLE_*
  // (§ "No texture swizzles") so any call with those pnames generates
  // INVALID_ENUM.  Kodi's shaders read the channels they need directly, so
  // no hardware-level swizzle remapping is required.
  attrs.proxyContextToMainThread = EMSCRIPTEN_WEBGL_CONTEXT_PROXY_FALLBACK;
  attrs.renderViaOffscreenBackBuffer = true;


  m_webglContext = emscripten_webgl_create_context("#canvas", &attrs);
  if (m_webglContext <= 0)
  {
    CLog::Log(LOGERROR, "WASM: emscripten_webgl_create_context failed ({})",
              m_webglContext);
    return false;
  }

  EMSCRIPTEN_RESULT r = emscripten_webgl_make_context_current(m_webglContext);
  if (r != EMSCRIPTEN_RESULT_SUCCESS)
  {
    CLog::Log(LOGERROR, "WASM: emscripten_webgl_make_context_current failed ({})", r);
    return false;
  }

  // OFFSCREEN_FRAMEBUFFER + PROXY_TO_PTHREAD: registerPreMainLoop() calls
  // GL.newRenderingFrameStarted() on this pthread each frame. Emscripten's
  // implementation swaps double-buffered temp VBOs used for client-side vertex
  // pointers (Kodi GUI). On the pthread, GL.currentContext may lack
  // tempVertexBufferCounters1 (crash) or the swap must apply to the main-thread
  // GL.contexts[handle] object — a full no-op leaves temp buffers unsynced and
  // can produce a black offscreen FBO. Mirror libwebgl.js newRenderingFrameStarted
  // when the full context is current; otherwise run the same logic on the main
  // thread via kodi_nfs_run_swap().
  EM_ASM({
    function kodiNfsSwap(ctx) {
      var vb = ctx.tempVertexBuffers1;
      ctx.tempVertexBuffers1 = ctx.tempVertexBuffers2;
      ctx.tempVertexBuffers2 = vb;
      vb = ctx.tempVertexBufferCounters1;
      ctx.tempVertexBufferCounters1 = ctx.tempVertexBufferCounters2;
      ctx.tempVertexBufferCounters2 = vb;
      var largestIndex = GL.log2ceilLookup(GL.MAX_TEMP_BUFFER_SIZE);
      for (var i = 0; i <= largestIndex; ++i) {
        ctx.tempVertexBufferCounters1[i] = 0;
      }
    }
    GL.newRenderingFrameStarted = function() {
      if (!GL.currentContext) return;
      if (GL.currentContext.tempVertexBufferCounters1) {
        kodiNfsSwap(GL.currentContext);
        return;
      }
      var h = GL.currentContext;
      if (typeof h !== 'number') h = h && h.handle;
      if (typeof h !== 'number') return;
      _kodi_nfs_run_swap(h);
    };
  });

  // Mirror the context current state to the browser main thread so that any
  // GL bookkeeping that runs there (extension queries, etc.) finds a valid ctx.
  MAIN_THREAD_EM_ASM({ GL.makeContextCurrent($0); }, m_webglContext);
  // Trace the effective runtime context mode (requested vs actual) to avoid
  // assuming OFFSCREEN_FRAMEBUFFER behavior from build flags alone.
  const int offscreenBackBuffer = MAIN_THREAD_EM_ASM_INT(
      {
        var ctx = GL.contexts[$0];
        if (!ctx || !ctx.attributes) return -1;
        return ctx.attributes.renderViaOffscreenBackBuffer ? 1 : 0;
      },
      m_webglContext);
  const int proxyMode = MAIN_THREAD_EM_ASM_INT(
      {
        var ctx = GL.contexts[$0];
        if (!ctx || !ctx.attributes) return -1;
        return ctx.attributes.proxyContextToMainThread || 0;
      },
      m_webglContext);
  const int explicitSwapControl = MAIN_THREAD_EM_ASM_INT(
      {
        var ctx = GL.contexts[$0];
        if (!ctx || !ctx.attributes) return -1;
        return ctx.attributes.explicitSwapControl ? 1 : 0;
      },
      m_webglContext);

  CLog::Log(LOGINFO,
            "WASM: WebGL context created (handle={}, requestedOffscreenBackBuffer=0, "
            "runtimeOffscreenBackBuffer={}, proxyMode={}, explicitSwapControl={})",
            m_webglContext, offscreenBackBuffer, proxyMode, explicitSwapControl);
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
  int w = res.iWidth;
  int h = res.iHeight;
  if (w <= 0 || h <= 0)
    GetCanvasSize(&w, &h);

  res.iWidth = w;
  res.iHeight = h;
  res.iScreenWidth = w;
  res.iScreenHeight = h;

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
  CRenderSystemGLES::ResetRenderSystem(newWidth, newHeight);
  return true;
}

bool CWinSystemWasmGLESContext::SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays)
{
  return CreateNewWindow("", fullScreen, res);
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
    const EMSCRIPTEN_RESULT makeCurrent =
        emscripten_webgl_make_context_current(m_webglContext);
    if (makeCurrent != EMSCRIPTEN_RESULT_SUCCESS)
    {
      CLog::Log(LOGWARNING, "WASM: PresentRenderImpl failed make_context_current ({})",
                makeCurrent);
      return;
    }
  }

  MAIN_THREAD_EM_ASM(
      {
        if (!Module.kodiWasmDiag && !Module.kodiWasmMaxDiag)
          return;
        if (!Module.__kodiWasmStateLogTick)
          Module.__kodiWasmStateLogTick = 0;
        Module.__kodiWasmStateLogTick++;
        var period = Module.kodiWasmMaxDiag ? 60 : 120;
        if (Module.__kodiWasmStateLogTick > 12 && (Module.__kodiWasmStateLogTick % period) !== 0)
          return;

        var ctx = GL.contexts[$0];
        if (!ctx || !ctx.GLctx)
          return;
        var gl = ctx.GLctx;
        var vp = gl.getParameter(gl.VIEWPORT);
        var sb = gl.getParameter(gl.SCISSOR_BOX);
        var cm = gl.getParameter(gl.COLOR_WRITEMASK);
        var cc = gl.getParameter(gl.COLOR_CLEAR_VALUE);
        var prog = gl.getParameter(gl.CURRENT_PROGRAM);
        var rb = gl.getParameter(gl.READ_FRAMEBUFFER_BINDING);
        var db = gl.getParameter(gl.DRAW_FRAMEBUFFER_BINDING);
        var readStatus = gl.checkFramebufferStatus(gl.READ_FRAMEBUFFER);
        var drawStatus = gl.checkFramebufferStatus(gl.DRAW_FRAMEBUFFER);
        var px = new Uint8Array(4);
        var prevSc = gl.isEnabled(gl.SCISSOR_TEST);
        if (prevSc) gl.disable(gl.SCISSOR_TEST);
        gl.readPixels((vp[2] / 2) | 0, (vp[3] / 2) | 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
        if (prevSc) gl.enable(gl.SCISSOR_TEST);
        var e = gl.getError();
        console.log('[kodi-wasm] pre-commit state viewport=' + Array.from(vp).join(',') +
                    ' scissorBox=' + Array.from(sb).join(',') +
                    ' colorMask=' + Array.from(cm).join(',') +
                    ' clear=' + Array.from(cc).map(function(v){return v.toFixed(2);}).join(',') +
                    ' scissor=' + gl.isEnabled(gl.SCISSOR_TEST) +
                    ' depth=' + gl.isEnabled(gl.DEPTH_TEST) +
                    ' stencil=' + gl.isEnabled(gl.STENCIL_TEST) +
                    ' blend=' + gl.isEnabled(gl.BLEND) +
                    ' cull=' + gl.isEnabled(gl.CULL_FACE) +
                    ' program=' + (!!prog) +
                    ' readFbo=' + (rb ? 'bound' : 'default') +
                    ' drawFbo=' + (db ? 'bound' : 'default') +
                    ' readStatus=0x' + readStatus.toString(16) +
                    ' drawStatus=0x' + drawStatus.toString(16) +
                    ' centerPx=' + Array.from(px).join(',') +
                    (e ? ' glErr=0x' + e.toString(16) : ""));
      },
      m_webglContext);

  MAIN_THREAD_EM_ASM(
      {
        if (!Module.kodiWasmDiag && !Module.kodiWasmMaxDiag)
          return;
        if (!Module.__kodiWasmPostUnbindTick)
          Module.__kodiWasmPostUnbindTick = 0;
        Module.__kodiWasmPostUnbindTick++;
        var period = Module.kodiWasmMaxDiag ? 60 : 120;
        if (Module.__kodiWasmPostUnbindTick > 12 && (Module.__kodiWasmPostUnbindTick % period) !== 0)
          return;
        var ctx = GL.contexts[$0];
        if (!ctx || !ctx.GLctx)
          return;
        var gl = ctx.GLctx;
        var rb = gl.getParameter(gl.READ_FRAMEBUFFER_BINDING);
        var db = gl.getParameter(gl.DRAW_FRAMEBUFFER_BINDING);
        var readStatus = gl.checkFramebufferStatus(gl.READ_FRAMEBUFFER);
        var drawStatus = gl.checkFramebufferStatus(gl.DRAW_FRAMEBUFFER);
        console.log('[kodi-wasm] post-unbind state readFbo=' + (rb ? 'bound' : 'default') +
                    ' drawFbo=' + (db ? 'bound' : 'default') +
                    ' readStatus=0x' + readStatus.toString(16) +
                    ' drawStatus=0x' + drawStatus.toString(16));
      },
      m_webglContext);

  // Diagnostic switch: force a visible clear right before commit to isolate
  // whether presentation works independently of Kodi's render content.
  MAIN_THREAD_EM_ASM(
      {
        if (!Module.kodiWasmForceClear)
          return;
        var ctx = GL.contexts[$0];
        if (!ctx || !ctx.GLctx)
          return;
        var gl = ctx.GLctx;
        gl.clearColor(1.0, 0.0, 1.0, 1.0);
        gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT | gl.STENCIL_BUFFER_BIT);
      },
      m_webglContext);

  const EMSCRIPTEN_RESULT commitResult = emscripten_webgl_commit_frame();
  if (EM_ASM_INT({ return Module.kodiWasmMaxDiag ? 1 : 0; }))
  {
    MAIN_THREAD_EM_ASM(
        { console.log('[kodi-wasm] commit rc=' + $0 + ' handle=' + $1); }, commitResult,
        m_webglContext);
  }
  if (commitResult != EMSCRIPTEN_RESULT_SUCCESS)
    CLog::Log(LOGWARNING, "WASM: emscripten_webgl_commit_frame failed ({})", commitResult);
}

int CWinSystemWasmGLESContext::GetBufferAge()
{
  return 0;
}
