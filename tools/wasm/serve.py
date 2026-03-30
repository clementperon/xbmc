#!/usr/bin/env python3
"""
Minimal HTTP server for Kodi WASM builds.

SharedArrayBuffer (required by Emscripten pthreads) needs these response headers:
  Cross-Origin-Opener-Policy: same-origin
  Cross-Origin-Embedder-Policy: require-corp

Usage:
  cd build-wasm
  cp ../tools/wasm/kodi.html .
  python3 ../tools/wasm/serve.py          # serves on http://localhost:8080
  python3 ../tools/wasm/serve.py 9000     # custom port
"""

import http.server
import sys


class CORPHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "application/javascript",
    }


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print(f"Serving on http://localhost:{port}  (Ctrl-C to stop)")
    print("Open http://localhost:{}/kodi.html in your browser.".format(port))
    http.server.HTTPServer(("", port), CORPHandler).serve_forever()
