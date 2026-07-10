# Kodi E2E test POC

Proof-of-concept end-to-end (E2E) test harness for Kodi, described in
**[docs/E2E-TESTING.md](../../docs/E2E-TESTING.md)**.

> [!NOTE]
> This is a proof of concept, not a complete test suite. It currently only proves the
> core mechanics: build Kodi from source, launch it with a disposable profile, confirm
> the JSON-RPC webserver responds, take and sanity-check a screenshot, and shut it down
> cleanly. See `docs/E2E-TESTING.md` for the full phased plan.

## What this does

Every scenario launches a built `kodi.bin`/`kodi` binary (via the shared `kodi`
fixture in `conftest.py`) with a throwaway `portable_data` profile (`-p`), pre-seeded
with the webserver enabled on a non-default port, authentication disabled, and a
screenshot output directory configured. Every scenario ends by asking Kodi to quit
via `driver/assertions.py`'s `assert_clean_shutdown`, which asserts a clean (`0`)
exit code and scans the log for `FATAL` lines - so a crash or hang fails loudly
regardless of which scenario was running.

`scenarios/test_startup.py`:

1. Waits for the webserver port to open.
2. Calls `JSONRPC.Ping` over HTTP JSON-RPC and asserts the `"pong"` response.

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

`scenarios/test_navigation.py`:

1. Pings, waits for the Home window, then calls `GUI.ActivateWindow(window="settings")`
   and waits for `currentwindow` to report the settings menu.
2. Calls `Input.Back` and waits for `currentwindow` to report Home again.

Deliberately uses `GUI.ActivateWindow`/`Input.Back` rather than scripted
`Input.Down`/`Input.Select` keypresses: which skin element is focused first is
skin/version-specific, so blindly navigating by direction would make the test fragile
in a way unrelated to what it's meant to catch. See the module docstring for the
reasoning.

`scenarios/test_settings.py`:

1. Pings, reads `lookandfeel.enablerssfeeds` via `Settings.GetSettingValue` and asserts
   it's the documented default (`false`).
2. Sets it to `true` via `Settings.SetSettingValue`, reads it back, and asserts the
   change persisted.

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
- `driver/assertions.py` — shared post-scenario assertions (currently just
  `assert_clean_shutdown`), used by every scenario.
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

- No test media / playback testing yet.
- No Linux/Windows CI wiring yet (only macOS, per this POC's scope).
- The screenshot check is a "did anything render at all" sanity check, not
  pixel/visual regression testing against a baseline (Phase 2).
- Binary add-ons are not built (`tools/depends/target/binary-addons` step is skipped),
  so this only proves core startup, not add-on-dependent functionality.
