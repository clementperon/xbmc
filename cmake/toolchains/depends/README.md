# Depends Toolchain Templates

`KodiDependsToolchain.cmake` is a generic CMake entrypoint for depends-driven
builds. It covers core environment handoff responsibilities from the generated
`tools/depends/target/Toolchain.cmake`:

- `DEPENDS_PATH` / `NATIVEPREFIX`
- `KODI_DEPENDSBUILD`
- cross-root search paths
- pkg-config and autotools environment bootstrap

It is intended for CMake-native orchestration and does not replace the generated
toolchain for all platform-specific details yet.
