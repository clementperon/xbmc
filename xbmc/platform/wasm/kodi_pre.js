// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Team Kodi
//
// Main-thread half of Kodi's WASM rendering pipeline: attaches an
// ImageBitmapRenderingContext to <canvas id="canvas"> and installs
// Module.onKodiFrame so the render pthread can deliver frames via
// postMessage({cmd:'callHandler', ...}).  See docs/wasm/RENDERING.md.

(function () {
  if (typeof document === 'undefined') {
    return;
  }

  var Module = globalThis.Module = globalThis.Module || {};
  var kodi = (Module.kodi = Module.kodi || {});

  var canvas = document.getElementById('canvas');
  if (!canvas) {
    console.warn('[kodi] No <canvas id="canvas"> found; rendering disabled.');
    return;
  }

  var bitmapCtx = null;
  try {
    bitmapCtx = canvas.getContext('bitmaprenderer', { alpha: false });
  } catch (e) {
    console.error('[kodi] Failed to get bitmaprenderer context:', e);
  }
  if (!bitmapCtx) {
    console.error(
        '[kodi] bitmaprenderer unsupported; falling back to 2D canvas blit.');
    bitmapCtx = canvas.getContext('2d');
  }
  kodi.canvas = canvas;
  kodi.bitmapCtx = bitmapCtx;

  // Only ever present the most recent bitmap; drop any older undisplayed one.
  var pendingBitmap = null;
  var pendingDrawScheduled = false;

  function presentPending() {
    pendingDrawScheduled = false;
    var bm = pendingBitmap;
    pendingBitmap = null;
    if (!bm) {
      return;
    }
    try {
      if (typeof bitmapCtx.transferFromImageBitmap === 'function') {
        bitmapCtx.transferFromImageBitmap(bm);
      } else {
        if (canvas.width !== bm.width || canvas.height !== bm.height) {
          canvas.width = bm.width;
          canvas.height = bm.height;
        }
        bitmapCtx.drawImage(bm, 0, 0);
        bm.close();
      }
    } catch (e) {
      console.error('[kodi] Failed to present frame:', e);
      try { bm.close(); } catch (_) {}
    }
  }

  Module.onKodiFrame = function (bitmap) {
    if (pendingBitmap) {
      try { pendingBitmap.close(); } catch (_) {}
    }
    pendingBitmap = bitmap;
    if (!pendingDrawScheduled) {
      pendingDrawScheduled = true;
      requestAnimationFrame(presentPending);
    }
  };

  var prevOnRuntime = Module.onRuntimeInitialized;
  Module.onRuntimeInitialized = function () {
    try { canvas.focus(); } catch (_) {}
    if (typeof prevOnRuntime === 'function') {
      try { prevOnRuntime(); } catch (e) { console.error(e); }
    }
  };
})();
