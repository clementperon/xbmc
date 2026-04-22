# Tizen web packaging template for Kodi WASM

This directory contains source-controlled template files used to stage a Tizen
web app from a WASM Kodi build.

## Files

- `config.xml`: Tizen widget metadata.
- `tizen_web_project.yaml`: Tizen web project config.
- `index.html`: Web entry page that loads `kodi.js`.

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
- Built WASM artifacts: `kodi.js`, `kodi.wasm`, `kodi.data`.

Package/sign/install to TV using Tizen CLI or Tizen Studio from the staged
directory.
