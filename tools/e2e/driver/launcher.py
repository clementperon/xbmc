"""Launches a Kodi binary with a disposable, pre-configured portable profile.

Kodi's "-p"/"--portable" flag makes it read/write its configuration from a
"portable_data" folder next to the executable instead of the platform's normal user
config location (see xbmc/application/AppParams.h UserDirectoriesLocation::PORTABLE
and xbmc/settings/SettingsComponent.cpp). We use that to give each test run a fresh,
disposable profile without touching any real Kodi installation, and pre-seed
guisettings.xml so the JSON-RPC webserver is already enabled on first launch.

Only exercised on macOS/Linux (POSIX) so far; Windows would need a different
termination fallback (no SIGTERM) if/when this is extended per docs/E2E-TESTING.md.
"""

from __future__ import annotations

import signal
import socket
import subprocess
import time
from pathlib import Path

DEFAULT_WEBSERVER_PORT = 18080  # Non-default port so this never collides with a real Kodi instance.

_GUISETTINGS_TEMPLATE = """<settings>
  <setting id="services.webserver">true</setting>
  <setting id="services.webserverport">{port}</setting>
  <setting id="services.webserverauthentication">false</setting>
</settings>
"""


class KodiProcess:
    def __init__(
        self,
        binary_path: str | Path,
        port: int = DEFAULT_WEBSERVER_PORT,
        startup_timeout: float = 120.0,
    ):
        self.binary_path = Path(binary_path).resolve()
        if not self.binary_path.exists():
            raise FileNotFoundError(
                f"Kodi binary not found at {self.binary_path}. Build Kodi first, or "
                "set the KODI_BINARY environment variable."
            )

        self.port = port
        self.startup_timeout = startup_timeout
        self.process: subprocess.Popen | None = None

    @property
    def portable_data_dir(self) -> Path:
        return self.binary_path.parent / "portable_data"

    @property
    def log_path(self) -> Path:
        return self.portable_data_dir / "temp" / "kodi.log"

    def _seed_settings(self) -> None:
        userdata_dir = self.portable_data_dir / "userdata"
        userdata_dir.mkdir(parents=True, exist_ok=True)
        settings_file = userdata_dir / "guisettings.xml"
        # Don't overwrite settings from a previous run in the same portable_data dir;
        # each test run should use a fresh directory anyway, but this keeps re-runs safe.
        if not settings_file.exists():
            settings_file.write_text(_GUISETTINGS_TEMPLATE.format(port=self.port))

    def start(self, extra_args: list[str] | None = None) -> None:
        self._seed_settings()

        args = [str(self.binary_path), "-p", "--debug"]
        if extra_args:
            args.extend(extra_args)

        self.process = subprocess.Popen(
            args,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self._wait_for_port()

    def _wait_for_port(self) -> None:
        deadline = time.monotonic() + self.startup_timeout
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError(
                    f"Kodi exited early (code {self.process.returncode}) while "
                    "waiting for the webserver to start. Check the captured log."
                )
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                sock.settimeout(1.0)
                if sock.connect_ex(("127.0.0.1", self.port)) == 0:
                    return
            time.sleep(1.0)
        raise TimeoutError(
            f"Kodi webserver did not open port {self.port} within "
            f"{self.startup_timeout}s"
        )

    def wait_for_exit(self, timeout: float = 30.0) -> int:
        """Waits for a Kodi process that has already been asked to quit.

        Falls back to SIGTERM then SIGKILL if it doesn't exit in time, so a hung
        process never leaks and hangs CI.
        """
        assert self.process is not None, "start() was not called"
        try:
            return self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            pass

        self.process.send_signal(signal.SIGTERM)
        try:
            return self.process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.process.kill()
            return self.process.wait(timeout=15)

    def read_log(self) -> str:
        if not self.log_path.exists():
            return ""
        return self.log_path.read_text(errors="replace")

    def kill_if_running(self) -> None:
        if self.process is not None and self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=15)
