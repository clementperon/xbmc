"""Minimal JSON-RPC over HTTP client for driving a running Kodi instance.

This is intentionally small: it only supports plain request/response calls, which is
enough for a startup ping/quit smoke test. It is not a replacement for a full Kodi
JSON-RPC client. If the E2E suite grows to need WebSocket notifications (e.g. waiting
for "Player.OnPlay"), prefer adopting a maintained library such as
`jsonrpc-websocket`/`pykodi` instead of extending this by hand - see
docs/E2E-TESTING.md for the rationale.
"""

from __future__ import annotations

import itertools
from typing import Any

import requests


class KodiJsonRpcError(RuntimeError):
    """Raised when Kodi's JSON-RPC endpoint returns an error response."""


class KodiJsonRpcClient:
    def __init__(self, host: str = "127.0.0.1", port: int = 18080, timeout: float = 10.0):
        self._url = f"http://{host}:{port}/jsonrpc"
        self._timeout = timeout
        self._ids = itertools.count(1)

    def call(self, method: str, params: dict[str, Any] | None = None) -> Any:
        payload: dict[str, Any] = {
            "jsonrpc": "2.0",
            "method": method,
            "id": next(self._ids),
        }
        if params is not None:
            payload["params"] = params

        response = requests.post(self._url, json=payload, timeout=self._timeout)
        response.raise_for_status()
        body = response.json()

        if "error" in body:
            raise KodiJsonRpcError(f"{method} failed: {body['error']}")
        return body.get("result")

    def ping(self) -> Any:
        """Calls JSONRPC.Ping, which should return the literal string "pong"."""
        return self.call("JSONRPC.Ping")

    def quit(self) -> Any:
        """Asks Kodi to shut down cleanly via Application.Quit."""
        return self.call("Application.Quit")

    def execute_action(self, action: str) -> Any:
        """Calls Input.ExecuteAction, e.g. execute_action("screenshot").

        Dispatched asynchronously as a GUI action message (same path as a keypress),
        so this returns before the action has necessarily finished - e.g. a screenshot
        is written by a background job, not synchronously by this call.
        """
        return self.call("Input.ExecuteAction", {"action": action})

    def current_window_id(self) -> int:
        """Returns the numeric id of the currently active GUI window.

        The webserver responding (JSONRPC.Ping) only means the JSON-RPC service
        thread is up, not that the skin has finished loading and drawn anything - the
        GUI initializes on a separate thread. Compare against a known window id (e.g.
        WINDOW_HOME = 10000, xbmc/guilib/WindowIDs.h) to detect actual GUI readiness
        before relying on rendered output such as a screenshot.
        """
        result = self.call("GUI.GetProperties", {"properties": ["currentwindow"]})
        return result["currentwindow"]["id"]
