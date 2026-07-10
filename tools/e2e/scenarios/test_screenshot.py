"""POC end-to-end rendering smoke test: launch Kodi, take a screenshot, sanity-check it.

This is not visual/pixel-regression testing (see docs/E2E-TESTING.md's Phase 2 for
that) - it only checks that something plausible got rendered at all, catching e.g. a
GL context that "succeeds" but draws nothing (a blank/solid-color frame).
"""

import time

import requests
from PIL import Image

from driver.kodi_client import KodiJsonRpcClient, KodiJsonRpcError
from driver.launcher import KodiProcess

WINDOW_HOME = 10000  # xbmc/guilib/WindowIDs.h
HOME_WINDOW_TIMEOUT = 60.0
# Generous: currentwindow flips to WINDOW_HOME the instant activation *starts*
# (GUIWindowManager.cpp adds it to the window history before sending the WINDOW_INIT
# message that actually parses Home.xml and loads its controls/textures), not once
# it's actually finished loading and drawn - so there's no reliable "done rendering"
# signal to poll for. Retry the screenshot itself instead, on a generous budget to
# absorb how slow the software-rendering fallback's first paint can be.
RENDER_TIMEOUT = 90.0
SCREENSHOT_TIMEOUT = 15.0  # per-attempt budget for one screenshot file to appear/settle
RETRY_INTERVAL = 2.0
MIN_EXPECTED_DIMENSION = 100  # sanity floor, well below any real Kodi window size


def _wait_for_home_window(client: KodiJsonRpcClient, timeout: float) -> None:
    """Waits for the GUI subsystem to at least reach the Home window.

    Not sufficient on its own as a "safe to screenshot now" signal (see module
    docstring above), but still a useful fast-fail gate: catches a GUI that never
    gets going at all, distinctly from one that's just slow to paint.
    """
    deadline = time.monotonic() + timeout
    last_window_id = None
    while time.monotonic() < deadline:
        try:
            last_window_id = client.current_window_id()
            if last_window_id == WINDOW_HOME:
                return
        except (KodiJsonRpcError, requests.exceptions.RequestException):
            pass
        time.sleep(0.5)
    raise TimeoutError(
        f"Home window (id {WINDOW_HOME}) not reached within {timeout}s "
        f"(last seen window id: {last_window_id})"
    )


def _wait_for_new_screenshot(kodi: KodiProcess, existing: set, timeout: float):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        new_files = set(kodi.screenshot_dir.glob("*.png")) - existing
        if new_files:
            screenshot = new_files.pop()
            # CScreenShot::TakeScreenshot writes the file (empty) before the background
            # CThumbnailWriter job fills it in, so wait for a stable, non-zero size
            # rather than returning the moment the (possibly still-empty) file appears.
            size = -1
            while time.monotonic() < deadline:
                current_size = screenshot.stat().st_size
                if current_size > 0 and current_size == size:
                    return screenshot
                size = current_size
                time.sleep(0.5)
            raise TimeoutError(
                f"{screenshot} appeared but never finished writing (stuck at "
                f"{size} bytes) within {timeout}s"
            )
        time.sleep(0.5)
    raise TimeoutError(f"No new screenshot appeared in {kodi.screenshot_dir} within {timeout}s")


def _capture_non_blank_screenshot(kodi: KodiProcess, client: KodiJsonRpcClient, timeout: float):
    deadline = time.monotonic() + timeout
    last_value = None
    while True:
        existing_screenshots = set(kodi.screenshot_dir.glob("*.png"))
        client.execute_action("screenshot")

        remaining = max(deadline - time.monotonic(), 1.0)
        screenshot_path = _wait_for_new_screenshot(
            kodi, existing_screenshots, min(SCREENSHOT_TIMEOUT, remaining)
        )

        with Image.open(screenshot_path) as image:
            image.verify()  # raises if the PNG is truncated/corrupt

        with Image.open(screenshot_path) as image:
            width, height = image.size
            assert width >= MIN_EXPECTED_DIMENSION and height >= MIN_EXPECTED_DIMENSION, (
                f"Screenshot {screenshot_path} is implausibly small ({width}x{height})"
            )

            # A single solid color (all-black being the classic symptom) means
            # something rendered a context but never actually drew the GUI into it.
            last_value = image.convert("L").getextrema()

        if last_value[0] != last_value[1]:
            return screenshot_path

        if time.monotonic() >= deadline:
            raise TimeoutError(
                f"No non-blank screenshot within {timeout}s - still a single solid "
                f"color (value {last_value[0]}) on the last attempt ({screenshot_path})"
            )
        time.sleep(RETRY_INTERVAL)


def test_screenshot_renders_non_blank_frame(kodi: KodiProcess):
    client = KodiJsonRpcClient(port=kodi.port)
    assert client.ping() == "pong", "JSONRPC.Ping did not return the expected 'pong'"

    _wait_for_home_window(client, HOME_WINDOW_TIMEOUT)
    _capture_non_blank_screenshot(kodi, client, RENDER_TIMEOUT)

    # Best-effort clean shutdown so Kodi's own log gets flushed to disk for CI
    # artifacts; the kodi fixture's SIGKILL fallback still applies if this doesn't
    # complete in time.
    try:
        client.quit()
    except requests.exceptions.RequestException:
        pass
    kodi.wait_for_exit(timeout=30)
