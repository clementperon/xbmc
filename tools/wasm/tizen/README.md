# Tizen web packaging template for Kodi WASM

Source-controlled template files used to stage a Tizen web app from a WASM Kodi
build.

The build, packaging and install workflow is documented in
[`docs/README.WASM.md`](../../../docs/README.WASM.md); SDK, device pairing and
certificate setup in [`docs/README.Tizen.md`](../../../docs/README.Tizen.md).
This file only describes what is in this directory and how it is staged.

## Files

- `icon.png`: application icon shown in the TV's Apps panel; 512x423 24-bit RGB PNG as Samsung requires, rendered from `tools/Linux/packaging/media/iconScalable.svg`.
- `config.xml`: Tizen widget metadata. Holds the application and package IDs
  (`kodiplayer.Kodi` / `kodiplayer`), the `tv-samsung` profile, the widget
  version and the privileges Kodi needs: `internet`, and the public-level
  Samsung `network.public` and `productinfo` privileges that let the System
  Information window show the TV's network configuration, model and firmware.
- `tizen_web_project.yaml.in`: CMake-configured Tizen web project config. Its
  `signing_profile` is intentionally empty, which makes `tz` sign with whichever
  security profile is active.
- `.project` / `.tproject`: Tizen Studio project descriptors, staged so the
  output directory can also be opened directly in the IDE.

The staged `index.html` is copied from the shared browser shell at
`tools/wasm/kodi.html` so browser and Tizen builds use the same entry page.

## Staging

`cmake --build <build-dir> --target package_tizen` assembles a complete Tizen
web project in `<build-dir>/tizen_packaging/Kodi`:

- the template files from this directory
- `tizen_web_project.yaml`, generated from the CMake template
- `index.html`, the shared web entry page
- `splash.jpg`, copied from `media/splash.jpg`
- the build artifacts `kodi.js`, `kodi.wasm` and `kodi.data`

`package_tizen_wgt` then packs and signs that directory into
`<build-dir>/tizen_packaging/Kodi/Debug/Kodi.wgt`, and `install_tizen_wgt`
pushes it to a connected device. Both require the `tz` CLI to have been found
when CMake configured the build; the values that name the staging directory,
output name, profile and API version live in
`cmake/scripts/wasm/ExtraTargets.cmake`.

## Profiling

`profiling/` holds a Chrome DevTools Protocol client and analysis scripts for
profiling Kodi on the TV from the command line, without the DevTools GUI. They
need Node 18 or later and no packages.

1. Configure the build with `-DENABLE_WASM_PROFILING=ON` so wasm function
   names survive into the profiles, build, and install with
   `cmake --build <build-dir> --target install_tizen_wgt`.
2. From the repository root run `tools/wasm/tizen/inspect.sh`; it launches
   Kodi under the Web Inspector and forwards the DevTools port to
   `localhost:7011`. If Kodi is already running, close it first with
   `node tools/wasm/tizen/profiling/kodiprof.mjs close`: `sdb shell 0 debug`
   hangs on a running app, and `Page.reload` does not bring Kodi back.
3. Attach right away with
   `node tools/wasm/tizen/profiling/kodiprof.mjs cpu-onplay 20 out/`. It
   attaches to the page and to every worker, waits until Kodi logs that
   WebCodecs playback has started, lets it settle and then samples every
   thread for 20 s into `out/<thread>.cpuprofile`. A worker another client
   has attached to cannot be attached again, so start the client before
   opening `chrome://inspect`. `cpu <seconds> [outdir]` profiles at once
   without waiting for playback.
4. `node tools/wasm/tizen/profiling/summarize_profiles.mjs out/` prints, per
   thread, wall and active time, the top self and inclusive functions, and
   what the thread was blocked on in futex waits.
   `compare_main.mjs before/main.cpuprofile after/main.cpuprofile` compares the
   browser main thread's time by category (commit blit, socket polling,
   decode, `copyTo`, texture uploads, other WebGL, idle) between two captures.

Other `kodiprof.mjs` modes: `console <seconds> [regex]` captures the console
output of the page and its workers, `screenshot <file.png>` grabs the canvas,
`eval <file.js>` evaluates an expression on the page and prints its value,
`probe` and `targets` show the DevTools endpoint and its targets. The snippets
in `profiling/eval/` are written for `eval` and read Kodi's log from the
Emscripten filesystem (`/home/web_user/.kodi/temp/kodi.log`, persisted in
IndexedDB):

| Snippet | Reports |
|---|---|
| `probe_tv.js` | browser, cores, canvas size, the `requestAnimationFrame` interval, `VideoDecoder.isConfigSupported` for common codecs with each hardware preference, `AudioContext` latencies |
| `playback_report.js` | for the last file played: `large audio sync error` count and samples, resync, caching and underrun lines, histogram of the intervals between presented frames |
| `cadence.js` | frame cadence from the A/V timing lines: frames presented, fps, wall-clock and pts-step histograms, skipped frames |
| `debug_logging_on.js`, `debug_logging_off.js` | switch debug logging with the audio, video and A/V timing components on in `guisettings.xml`, and back to the defaults; Kodi reads the file at start, so `close` and relaunch afterwards |

The TV's DevTools server refuses WebSocket handshakes that carry an `Origin`
header, which is why `kodiprof.mjs` implements the handshake over `node:http`
itself rather than using Node's `WebSocket`.
