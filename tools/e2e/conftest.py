import os
import pathlib
import sys

import pytest

# Allow "from driver.xxx import yyy" regardless of the directory pytest is invoked from.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from driver.launcher import KodiProcess  # noqa: E402

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
DEFAULT_KODI_BINARY = REPO_ROOT / "build" / "kodi.bin"


def _kodi_binary_path() -> pathlib.Path:
    return pathlib.Path(os.environ.get("KODI_BINARY", DEFAULT_KODI_BINARY))


@pytest.fixture
def kodi():
    proc = KodiProcess(_kodi_binary_path())
    proc.start()
    try:
        yield proc
    finally:
        proc.kill_if_running()
