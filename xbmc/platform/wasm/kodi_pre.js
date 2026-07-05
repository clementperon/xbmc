// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Team Kodi
//
// Main-thread half of Kodi's WASM rendering pipeline: attaches an
// ImageBitmapRenderingContext to <canvas id="canvas"> and installs
// Module.onKodiFrame so the render pthread can deliver frames via
// postMessage({cmd:CMD_CALL_HANDLER, ...}) (see
// WinSystemWasmGLESContext.cpp::PostFrameBitmap for why that's a numeric
// id, not the string it used to be).  See docs/wasm/RENDERING.md.
//
// Also installs a same-origin HTTP proxy shim (see tools/wasm/serve.py).
// Any cross-origin http(s) XHR/fetch issued from the wasm module is
// rewritten to Module.kodiHttpProxy + '?u=<encoded>' so it bypasses CORS
// in the browser dev setup. Set Module.kodiHttpProxy = null before module
// startup to disable (e.g. on Tizen, where the web runtime allows
// cross-origin XHR directly).

(function installHttpProxy(scope) {
  var Module = (scope.Module = scope.Module || {});
  if (Module.kodiHttpProxy === undefined) {
    Module.kodiHttpProxy = '/proxy';
  }

  function rewrite(url) {
    var base = Module.kodiHttpProxy;
    if (!base || typeof url !== 'string') {
      return url;
    }
    try {
      var u = new URL(url, scope.location.href);
      if (u.protocol !== 'http:' && u.protocol !== 'https:') {
        return url;
      }
      if (u.origin === scope.location.origin) {
        return url;
      }
      return base + '?u=' + encodeURIComponent(u.href);
    } catch (_) {
      return url;
    }
  }

  if (scope.XMLHttpRequest && scope.XMLHttpRequest.prototype &&
      !scope.XMLHttpRequest.prototype.__kodiProxyPatched) {
    var origOpen = scope.XMLHttpRequest.prototype.open;
    scope.XMLHttpRequest.prototype.open = function (method, url) {
      arguments[1] = rewrite(url);
      return origOpen.apply(this, arguments);
    };
    scope.XMLHttpRequest.prototype.__kodiProxyPatched = true;
  }

  if (typeof scope.fetch === 'function' && !scope.fetch.__kodiProxyPatched) {
    var origFetch = scope.fetch.bind(scope);
    var patched = function (input, init) {
      if (typeof input === 'string') {
        input = rewrite(input);
      } else if (input && typeof input.url === 'string') {
        var rewritten = rewrite(input.url);
        if (rewritten !== input.url) {
          input = new Request(rewritten, input);
        }
      }
      return origFetch(input, init);
    };
    patched.__kodiProxyPatched = true;
    scope.fetch = patched;
  }
})(globalThis);

// Persist Kodi's user profile ($HOME/.kodi) to IndexedDB via IDBFS so that
// sources.xml, guisettings.xml, the databases, etc. survive a page refresh.
// Mount and populate run inside Module.preRun, and addRunDependency blocks
// main() until IDBFS has finished loading from IndexedDB.
(function installIdbfsPersistence() {
  var Module = globalThis.Module = globalThis.Module || {};
  var PROFILE_PATH = '/home/web_user/.kodi';

  Module.preRun = Module.preRun || [];
  Module.preRun.push(function mountKodiProfile() {
    try {
      FS.mkdirTree(PROFILE_PATH);
    } catch (e) {
      console.warn('[kodi] mkdirTree ' + PROFILE_PATH + ':', e);
    }

    try {
      FS.mount(IDBFS, {}, PROFILE_PATH);
    } catch (e) {
      console.warn('[kodi] IDBFS mount failed, profile will not persist:', e);
      return;
    }

    // Block main() until the profile has been loaded from IndexedDB.
    addRunDependency('kodi-idbfs-populate');
    FS.syncfs(true, function (err) {
      if (err) {
        console.warn('[kodi] IDBFS populate:', err);
      }
      removeRunDependency('kodi-idbfs-populate');
    });

    // Debounced background flush to IndexedDB.
    var syncing = false;
    var pending = false;
    function flushToIdb() {
      if (syncing) { pending = true; return; }
      syncing = true;
      FS.syncfs(false, function (err) {
        syncing = false;
        if (err) { console.warn('[kodi] IDBFS persist:', err); }
        if (pending) { pending = false; flushToIdb(); }
      });
    }

    setInterval(flushToIdb, 5000);

    if (typeof window !== 'undefined') {
      window.addEventListener('pagehide', flushToIdb, { capture: true });
      window.addEventListener('beforeunload', flushToIdb, { capture: true });
    }
  });
})();

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
  // tabindex=0 allows the canvas to receive focus so paste and keyboard reach Kodi.
  if (canvas.getAttribute('tabindex') === '-1' || canvas.getAttribute('tabindex') === null) {
    canvas.setAttribute('tabindex', '0');
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
    // Must run on the browser main thread (document is undefined on pthread workers).
    document.addEventListener(
        'paste',
        function (e) {
          try {
            var text = (e.clipboardData && e.clipboardData.getData)
                ? e.clipboardData.getData('text/plain')
                : String();
            if (text === undefined || text === null) {
              text = String();
            }
            e.preventDefault();
            if (typeof Module.ccall === 'function') {
              Module.ccall('kodi_wasm_dispatch_paste', null, ['string'], [text]);
            }
          } catch (err) {
            console.error('[kodi] paste handler:', err);
          }
        },
        true);
    try { canvas.focus(); } catch (_) {}
    if (typeof prevOnRuntime === 'function') {
      try { prevOnRuntime(); } catch (e) { console.error(e); }
    }
  };
})();
