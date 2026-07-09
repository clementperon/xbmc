"""POC end-to-end rendering smoke test: launch Kodi, take a screenshot, sanity-check it.

This is not visual/pixel-regression testing (see docs/E2E-TESTING.md's Phase 2 for
that) - it only checks that something plausible got rendered at all, catching e.g. a
GL context that "succeeds" but draws nothing (a blank/solid-color frame).
"""

import time

from PIL import Image

from driver.kodi_client import KodiJsonRpcClient
from driver.launcher import KodiProcess

SCREENSHOT_TIMEOUT = 15.0
MIN_EXPECTED_DIMENSION = 100  # sanity floor, well below any real Kodi window size


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
            return screenshot
        time.sleep(0.5)
    raise TimeoutError(f"No new screenshot appeared in {kodi.screenshot_dir} within {timeout}s")


def test_screenshot_renders_non_blank_frame(kodi: KodiProcess):
    client = KodiJsonRpcClient(port=kodi.port)
    assert client.ping() == "pong", "JSONRPC.Ping did not return the expected 'pong'"

    existing_screenshots = set(kodi.screenshot_dir.glob("*.png"))
    client.execute_action("screenshot")

    screenshot_path = _wait_for_new_screenshot(kodi, existing_screenshots, SCREENSHOT_TIMEOUT)

    with Image.open(screenshot_path) as image:
        image.verify()  # raises if the PNG is truncated/corrupt

    with Image.open(screenshot_path) as image:
        width, height = image.size
        assert width >= MIN_EXPECTED_DIMENSION and height >= MIN_EXPECTED_DIMENSION, (
            f"Screenshot {screenshot_path} is implausibly small ({width}x{height})"
        )

        # A single solid color (all-black being the classic symptom) means something
        # rendered a context but never actually drew the GUI into it.
        extrema = image.convert("L").getextrema()
        assert extrema[0] != extrema[1], (
            f"Screenshot {screenshot_path} is a single solid color (value {extrema[0]}) - "
            "nothing appears to have been rendered"
        )
