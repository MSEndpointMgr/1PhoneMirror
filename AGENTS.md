# AGENTS.md — 1PhoneMirror

C++20 Windows screen-mirroring receiver (AirPlay, Android/scrcpy, Miracast, Cast).
Single-binary SDL2 + FFmpeg desktop app, plus a small C# Azure Functions
telemetry backend under [telemetry/](telemetry/).

For user-facing docs see [README.md](README.md), [GETTING_STARTED.md](GETTING_STARTED.md),
[PRIVACY.md](PRIVACY.md), [GOVERNANCE.md](GOVERNANCE.md), [DISTRIBUTION.md](DISTRIBUTION.md).

## Build & run

```powershell
# First time only — installs vcpkg, FFmpeg, SDL2, OpenSSL
.\scripts\setup_deps.ps1
# Every build (Release by default)
.\scripts\build.ps1                  # add -Config Debug / -Clean / -NoAirPlay / -NoMiracast
.\build\Release\1PhoneMirror.exe
```

The exe name on disk is `1PhoneMirror.exe`; older docs/scripts sometimes spell
it lowercase — Windows treats them as equal but match the CMake target
(`1PhoneMirror`) in new code.

There is **no automated test suite**. Closest thing to a unit test is
`1PhoneMirror.exe --srp-self-test` (returns 0 on success). Always smoke-test
a Release build before tagging.

## Repository layout

| Path | Contents |
|---|---|
| [include/opm/](include/opm) | Public headers — namespace `opm::`, one subdir per module (`airplay/`, `android/`, `cast/`, `media/`, `miracast/`, `network/`). |
| [src/](src) | Implementations mirror the header layout. `main.cpp` → CLI/startup; `app.cpp` → `opm::App` glues renderer + receivers. |
| [lib/](lib) | Vendored sources: `stb_image*` and `playfair/` (AirPlay FairPlay). Do not reformat. |
| [resources/](resources) | `.rc`, app manifest, icons embedded into the exe. |
| [installer/](installer) | WiX 5 `.wxs` source + `THIRD_PARTY_LICENSES.txt`. |
| [scripts/](scripts), [package.ps1](package.ps1) | Build + MSI/Intune packaging. |
| [telemetry/](telemetry) | **Separate** .NET 8 Azure Functions project — deployed via `azd up`, not part of the C++ build. See [telemetry/README.md](telemetry/README.md). |
| [manifests/](manifests) | winget manifest fork copy (publish via [.github/workflows/winget.yml](.github/workflows/winget.yml)). |
| [docs/screenshots/](docs/screenshots) | README screenshots only. |

## Architecture rules

- **One protocol streams at a time.** `opm::App::active_source_` is an atomic
  enum (`None | AirPlay | Miracast | Cast | Android`). Receiver callbacks must
  check `active_source_` before pushing frames to the renderer/audio.
- **Per-protocol gating.** Every protocol's headers, sources, link libs, and
  call sites are wrapped in `#ifdef ENABLE_AIRPLAY` / `_MIRACAST` / `_CAST` /
  `_ANDROID`. The matching `option()` in [CMakeLists.txt](CMakeLists.txt)
  controls both the source list and the compile definition — keep them in
  sync when adding files.
- **No blocking on the SDL event/render thread.** Disconnects, `adb` shells,
  and anything that joins worker threads must run on a detached `std::thread`
  (see the `airplay_.disconnect_source` / `android_disconnect` call sites in
  `src/app.cpp`). Blocking the UI thread surfaces as "Not Responding" and
  leaves iOS unable to reconnect.
- **Multi-device support.** AirPlay can hold multiple sources; Android keeps
  one `AndroidSession` per serial in `scrcpy_sessions_`. The bottom-bezel dot
  picker is driven by `App::source_order_` — preserve insertion order.

## Conventions

- **Version is set in ONE place:** the `project(... VERSION x.y.z ...)` line
  in [CMakeLists.txt](CMakeLists.txt). It feeds `OPM_VERSION_{MAJOR,MINOR,PATCH}`
  compile defs and is parsed by [package.ps1](package.ps1) for the MSI name.
  When bumping it, **also add a row to the version-history table in
  [README.md](README.md)** — the in-app "Version history" panel (`V`) is
  rendered from that same table.
- **Logging:** plain `std::cout` / `std::cerr` with a `[Module] ` prefix
  (`[App]`, `[AirPlay]`, `[Shutdown]`, `[CRASH]`, …). `opm::LogBuffer` tees
  stdout/stderr into the in-app log viewer (`L` key); do not introduce a
  separate logging framework.
- **Settings persistence:** add fields to `opm::Settings` in
  [include/opm/settings.h](include/opm/settings.h), then extend the parser
  and writer in [src/settings.cpp](src/settings.cpp). File format is plain
  `key=value` lines in `%APPDATA%\1PhoneMirror\settings.ini`. Always provide
  a sensible default so existing users don't lose their config.
- **Strings stay ASCII** in code and CLI output (the log viewer renders with
  SDL_ttf; non-ASCII glyphs show as boxes).
- **Includes:** use `<opm/...>` for project headers (configured as
  `target_include_directories(... PRIVATE include)`).

## Known pitfalls (already coded around — don't undo)

- **Stale process / port conflict.** `kill_stale_instances()` in
  [src/main.cpp](src/main.cpp) terminates leftover `1PhoneMirror.exe`
  processes before binding ports 7000/7100 and registering mDNS. Required
  after a crash.
- **`adb.exe` squats on UDP 5353.** `kill_adb_processes()` in
  [src/app.cpp](src/app.cpp) force-kills all `adb.exe` instances on shutdown
  so Bonjour can rebind mDNS on the next launch.
- **Firewall prompt.** `check_firewall_rules()` creates inbound TCP 7000/7100,
  UDP 5353/7010/7011 and a program-level rule via an elevated `netsh`. Must
  remain idempotent.
- **TripIt 1920×1080 AirPlay surface.** The renderer letterboxes it inside
  the phone frame instead of auto-rotating — keep that behaviour.
- **WiX 5 extensions** must be pinned to the same major version as the
  installed `wix` tool, e.g. `wix extension add -g WixToolset.Firewall.wixext/5.*`.
  Otherwise the default `5.x` resolves to a v7 package and the build fails.
- **`SUBSYSTEM:WINDOWS` + `mainCRTStartup`.** The link flags in
  [CMakeLists.txt](CMakeLists.txt) deliberately combine a Windows-subsystem
  binary with the console `main()` entry point so the EXE has no console
  window but still uses the standard `int main(int, char**)` signature.

## Packaging & release

- **MSI:** `.\package.ps1` — builds Release, stages DLLs + VC++ runtime,
  invokes WiX 5, optionally signs via `-SignCertThumbprint` (local signtool).
  `-IntuneWinAppUtil <path>` wraps the MSI for Intune. Releases are
  currently published unsigned; see [GOVERNANCE.md](GOVERNANCE.md#release-signing).
- **CI release:** push a `v*.*.*` tag on `main` → [.github/workflows/release.yml](.github/workflows/release.yml)
  builds + uploads the MSI as a GitHub Release asset.
- **winget:** [.github/workflows/winget.yml](.github/workflows/winget.yml)
  publishes `MSEndpointMgr.1PhoneMirror`.

## Telemetry subproject ([telemetry/](telemetry))

Independent .NET 8 isolated-worker Functions app (`PingFunction`,
`DownloadFunction`, `HealthFunction`). Deploy with `azd up` from
[telemetry/](telemetry); do not couple it to the C++ build. Payload
schema and what's stored is documented in [telemetry/README.md](telemetry/README.md)
and [PRIVACY.md](PRIVACY.md) — keep them in sync if you change the JSON
shape or add a field.
