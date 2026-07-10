# Kodi E2E test POC

Proof-of-concept end-to-end (E2E) test harness for Kodi, described in
**[docs/E2E-TESTING.md](../../docs/E2E-TESTING.md)**.

> [!NOTE]
> This is a proof of concept, not a complete test suite. It currently only proves the
> core mechanics: build Kodi from source, launch it with a disposable profile, confirm
> the JSON-RPC webserver responds, take and sanity-check a screenshot, and shut it down
> cleanly. See `docs/E2E-TESTING.md` for the full phased plan.

## What this does

Both scenarios launch a built `kodi.bin`/`kodi` binary (via the shared `kodi` fixture
in `conftest.py`) with a throwaway `portable_data` profile (`-p`), pre-seeded with the
webserver enabled on a non-default port, authentication disabled, and a screenshot
output directory configured.

`scenarios/test_startup.py`:

1. Waits for the webserver port to open.
2. Calls `JSONRPC.Ping` over HTTP JSON-RPC and asserts the `"pong"` response.
3. Calls `Application.Quit` and waits for the process to exit.
4. Asserts a clean (`0`) exit code.
5. Scans the Kodi log for `FATAL` lines and fails the test if any are found.

`scenarios/test_screenshot.py`:

1. Pings, then waits for `GUI.GetProperties(currentwindow)` to report the Home window -
   a coarse fast-fail gate, not a "finished rendering" signal (Kodi flips
   `currentwindow` the instant window activation *starts*, before that window's XML
   layout/controls/textures actually load - see the module docstring).
2. Triggers a screenshot via `Input.ExecuteAction` (`{"action": "screenshot"}`, the
   same action a "screenshot" keybinding would send), waits for the resulting PNG to
   appear and finish being written, and checks it isn't a single solid color. Since
   there's no reliable "done rendering" signal to wait for, this retries on a generous
   budget rather than asserting on the first attempt - the software-rendering
   fallback's first paint can be slow.
3. Asserts the final screenshot is a valid, plausibly-sized image that isn't a single
   solid color - catching e.g. a GL context that creates successfully but renders
   nothing. This is a basic sanity check, not pixel/visual regression testing (Phase 2 in
   `docs/E2E-TESTING.md`).

## Layout

- `conftest.py` — the shared `kodi` fixture (launches/tears down a `KodiProcess` per
  test) and `KODI_BINARY` resolution, used by every scenario.
- `driver/launcher.py` — starts/stops a Kodi process with a disposable portable
  profile. Currently only exercised on macOS/Linux (POSIX) in CI; not Kodi/OS-specific
  beyond that.
- `driver/kodi_client.py` — a minimal HTTP JSON-RPC client (no external Kodi-specific
  dependency). As the suite grows past simple request/response calls (e.g. waiting on
  `Player.OnPlay` notifications), see `docs/E2E-TESTING.md` for the recommendation to
  adopt `jsonrpc-websocket`/`pykodi` instead of extending this by hand.
- `scenarios/` — the actual pytest test cases.

## Running locally

Build Kodi first (see `docs/README.macOS.md` / `docs/README.Linux.md`), then, using
[`uv`](https://docs.astral.sh/uv/) (dependencies are declared in `pyproject.toml` /
`uv.lock`, no manual virtualenv setup needed):

```bash
cd tools/e2e
KODI_BINARY=/path/to/kodi.bin uv run pytest scenarios -v
```

If `KODI_BINARY` is not set, it defaults to `<repo_root>/build/kodi.bin`.

## CI

Wired up for macOS only so far, as a proof of concept, via
`.github/workflows/e2e-poc-macos.yml`. It builds Kodi from source on a GitHub-hosted
macOS runner (caching `tools/depends` output and `ccache` to keep repeat runs fast),
then runs this test suite against the freshly built binary. It's currently
manually-triggered (`workflow_dispatch`) rather than wired into every PR, since build
time and reliability haven't been proven out yet.

## Known limitations / not yet covered

- No test media / playback testing yet (ping + quit, and a startup screenshot, only).
- No Linux/Windows CI wiring yet (only macOS, per this POC's scope).
- The screenshot check is a "did anything render at all" sanity check, not
  pixel/visual regression testing against a baseline (Phase 2).
- Binary add-ons are not built (`tools/depends/target/binary-addons` step is skipped),
  so this only proves core startup, not add-on-dependent functionality.
