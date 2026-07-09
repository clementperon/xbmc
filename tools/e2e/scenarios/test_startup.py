"""POC end-to-end smoke test: launch Kodi, ping it over JSON-RPC, quit cleanly.

This is the minimal first slice of docs/E2E-TESTING.md: it doesn't touch playback,
navigation, or any GUI interaction, only confirms the built binary starts up, the
JSON-RPC webserver responds, and it shuts down cleanly without fatal errors.
"""

import re

import requests

from driver.kodi_client import KodiJsonRpcClient
from driver.launcher import KodiProcess

FATAL_LOG_LINE = re.compile(r"\bFATAL\b")


def test_startup_ping_and_quit(kodi: KodiProcess):
    client = KodiJsonRpcClient(port=kodi.port)

    assert client.ping() == "pong", "JSONRPC.Ping did not return the expected 'pong'"

    # Kodi tears down the webserver as part of quitting, so the HTTP response to the
    # quit call itself may never arrive cleanly - that's expected, not a test failure.
    try:
        client.quit()
    except requests.exceptions.RequestException:
        pass

    exit_code = kodi.wait_for_exit(timeout=30)
    log_text = kodi.read_log()

    assert exit_code == 0, (
        f"Kodi did not exit cleanly (code {exit_code}). Log tail:\n"
        + "\n".join(log_text.splitlines()[-40:])
    )

    fatal_lines = [line for line in log_text.splitlines() if FATAL_LOG_LINE.search(line)]
    assert not fatal_lines, f"Found FATAL log line(s): {fatal_lines}"
