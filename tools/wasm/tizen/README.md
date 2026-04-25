# Tizen web packaging template for Kodi WASM

This directory contains source-controlled template files used to stage a Tizen
web app from a WASM Kodi build.

## Files

- `config.xml`: Tizen widget metadata.
- `tizen_web_project.yaml`: Tizen web project config.

The staged `index.html` is copied from the shared browser shell at
`tools/wasm/kodi.html` so browser and Tizen builds use the same entry page.

## CMake staging target

When building the `wasm` platform, run:

```bash
cmake --build <build-dir> --target package_tizen
```

This creates a staging directory at:

```text
<build-dir>/packaging/tizen
```

The stage includes:

- Tizen template files from this directory.
- Shared web entry page staged as `index.html`.
- Built WASM artifacts: `kodi.js`, `kodi.wasm`, `kodi.data`.

Package/sign/install to TV using Tizen CLI or Tizen Studio from the staged
directory.

## Direct WGT packaging target

If `tz` is available, CMake also exposes:

```bash
cmake --build <build-dir> --target package_tizen_wgt
```

`tz` is discovered from environment variables first:

- `TIZEN_CLI_PATH`
- `TIZEN_TOOLS_PATH`
- `TIZEN_SDK`
- `TIZEN_SDK_ROOT`

## Install target

If `tz` is available, CMake exposes:

```bash
cmake --build <build-dir> --target install_tizen_wgt
```

This runs `tz install --package-path <build-dir>/packaging/tizen/Debug/tizen.wgt`
and uses one of:

- `WASM_TIZEN_INSTALL_TARGET` (or env `TIZEN_TARGET_NAME`)
- `WASM_TIZEN_INSTALL_SERIAL` (or env `TIZEN_TARGET_SERIAL`)
