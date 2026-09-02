# Tizen web packaging template for Kodi WASM

This directory contains source-controlled template files used to stage a Tizen
web app from a WASM Kodi build.

## Files

- `config.xml`: Tizen widget metadata.
- `tizen_web_project.yaml.in`: CMake-configured Tizen web project config.
- `.project` / `.tproject`: Tizen Studio project descriptors, staged so the
  output directory can also be opened directly in the IDE.

The staged `index.html` is copied from the shared browser shell at
`tools/wasm/kodi.html` so browser and Tizen builds use the same entry page.

## CMake staging target

When building the `wasm` platform, run:

```bash
cmake --build <build-dir> --target package_tizen
```

This creates a staging directory at:

```text
<build-dir>/tizen_packaging/Kodi
```

The stage includes:

- Tizen template files from this directory.
- Generated `tizen_web_project.yaml` from the CMake template.
- Shared web entry page staged as `index.html`.
- `splash.jpg`, copied from `media/splash.jpg`.
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

The signed package is written to:

```text
<build-dir>/tizen_packaging/Kodi/Debug/Kodi.wgt
```

## Signing profile

`tizen_web_project.yaml.in` leaves `signing_profile` empty, which means `tz`
signs with whichever profile is currently active. Retail Samsung TVs reject
packages that are not signed with a Samsung distributor certificate bound to
the target device's DUID, so a profile has to exist before `package_tizen_wgt`
produces something installable:

```bash
tz security-profiles list
```

Creating one needs the Samsung Certificate Extension (Tizen Studio Package
Manager, "Extension SDK" tab). `tz cert` creates the author certificate; the
DUID-bound distributor certificate comes from the Samsung Certificate Manager
and requires a Samsung account. Read the DUID off a connected device with:

```bash
sdb shell 0 getduid
```

Register the resulting certificates as an active profile:

```bash
tz security-profiles add -n <profile> -A \
  -a <author.p12> -p <author-password> \
  -d <distributor.p12> -P <distributor-password>
```

The package ID prefix in `config.xml` (`<tizen:application package="...">`)
must match the one the distributor certificate was issued for.

## Install target

If `tz` is available, CMake exposes:

```bash
cmake --build <build-dir> --target install_tizen_wgt
```

This runs `tz install --package-path <build-dir>/tizen_packaging/Kodi/Debug/Kodi.wgt`.

With more than one device connected, name the one to install to using either:

- `WASM_TIZEN_INSTALL_SERIAL` (or env `TIZEN_TARGET_SERIAL`), passed as `--serial`
- `WASM_TIZEN_INSTALL_TARGET` (or env `TIZEN_TARGET_NAME`), passed as `--target`

The serial takes precedence when both are set. Values are read at configure
time, so change them with `cmake -D...` on an existing build directory rather
than exporting into the build shell.

A TV must be paired over `sdb connect <tv-ip>` and listed by `sdb devices`
before either form resolves.

Launching and removing the installed app go through `tz` directly, and the two
take the package ID in different forms:

```bash
tz run -p kodiplayer              # package ID alone
tz uninstall -p kodiplayer.Kodi   # package ID and yaml output_name
```

Neither accepts the `<tizen:application>` ID from `config.xml`.
