# 1PhoneMirror

An open-source screen-mirroring receiver for Windows that lets an iPhone or
an Android phone show up inside a phone-shaped window on your PC — no app
installed on the phone, no cables.

- **iOS / iPadOS / macOS** — native AirPlay (Screen Mirroring), with optional PIN pairing.
- **Android** — Wireless Debugging via the bundled `adb` + `scrcpy-server.jar`.
- **Miracast** — Wi-Fi Direct receiver (experimental, Windows-only).

Multiple phones can stay paired at once and appear as small dots in the
bottom bezel; left-click switches the active source, right-click opens a
menu to disconnect.

## Architecture

```
                ┌────────────────────────────────────────────┐
                │             1PhoneMirror.exe               │
                └────────────────────────────────────────────┘
                          │            │            │
        ┌─────────────────┘            │            └─────────────────┐
        ▼                              ▼                              ▼
┌────────────────┐         ┌────────────────────┐         ┌──────────────────────┐
│  AirPlay 2     │         │  Android (scrcpy)  │         │  Miracast (WinRT)    │
│  RTSP + mDNS   │         │  ADB pair/connect  │         │  Wi-Fi Direct        │
│  FairPlay AES  │         │  scrcpy v3 stream  │         │  Wi-Fi Display       │
└───────┬────────┘         └─────────┬──────────┘         └──────────┬───────────┘
        │ H.264 / AAC                │ H.264 (Annex-B)               │ H.264
        └────────────────────────────┼───────────────────────────────┘
                                     ▼
                       ┌──────────────────────────┐
                       │  FFmpeg decode (sw/hw)   │  YUV → RGBA
                       └─────────────┬────────────┘
                                     ▼
                       ┌──────────────────────────┐
                       │   SDL2 renderer          │  phone frame, island
                       │   + Win32 GDI text       │  menu, log viewer,
                       │   + audio output         │  source dots, panels
                       └──────────────────────────┘
```

Source layout:

| Path | Contents |
|------|----------|
| `src/airplay/` | mDNS service, RTSP server, AirPlay mirror session, FairPlay |
| `src/android/` | `AdbController` (Win32 process pipes), scrcpy v3 stream receiver, in-app pair/connect dialog |
| `src/miracast/` | WinRT Miracast receiver |
| `src/media/` | FFmpeg decoder, SDL2 renderer, audio output, phone-frame overlay |
| `src/network/` | TCP / RTSP plumbing |
| `src/app.cpp` | Wires sources to renderer, owns lifecycle |
| `tools/adb/` | Bundled platform-tools `adb.exe` (staged at build time) |
| `tools/scrcpy-server.jar` | scrcpy v3.0 server, pushed to the phone over ADB |

## Prerequisites

- Windows 10 1903+ (Miracast) / Windows 11 recommended
- Visual Studio 2022 with the C++ Desktop workload (or the Build Tools)
- CMake 3.20+
- vcpkg (the build script uses `$env:VCPKG_ROOT` or `%USERPROFILE%\vcpkg`)

## Build

```powershell
.\scripts\setup_deps.ps1   # one-time vcpkg install of FFmpeg, SDL2, OpenSSL
.\scripts\build.ps1
.\build\Release\1PhoneMirror.exe
```

The build script also stages `tools\adb\adb.exe` and `tools\scrcpy-server.jar`
into `build\Release\tools\` so the Android receiver works out of the box.

## Connecting a phone

### iPhone / iPad / Mac (AirPlay)
1. PC and phone on the **same Wi-Fi**.
2. Phone: **Control Center → Screen Mirroring → 1PhoneMirror**.
3. If PIN pairing is enabled, type the PIN shown on the PC.

### Android (Wireless Debugging)
1. Phone: **Settings → Developer options → Wireless debugging** → enable.
2. Tap **Pair device with pairing code** — note the **IP : port** and **6-digit code**.
3. PC: in 1PhoneMirror press **A** — the dialog pre-fills your PC's `/24`
   subnet (e.g. `192.168.0.`); type the phone's last octet, the pair port,
   and the PIN, then **Connect**. The phone shows up via mDNS within a
   second or two and starts mirroring.

A new pairing code is required each time the pair-with-code screen is
opened. Once paired, the device serial sticks until you forget it on
the phone.

### Miracast (Android, experimental)
1. PC: leave Miracast enabled (default).
2. Phone: **Quick Settings → Cast / Smart View / Screen Cast** → pick the PC.

## Keyboard shortcuts

| Key | Action |
|-----|--------|
| `F` | Toggle fullscreen |
| `M` | Toggle island menu |
| `L` | Toggle log viewer |
| `A` | Open Android pair / connect dialog |
| `I` | Toggle info panel |
| `V` | Toggle version history |
| `P` | Toggle phone-frame overlay |
| `Ctrl+S` | Screenshot to clipboard + Pictures folder |
| `Esc` | Close panel / quit |

## Command-line options

```
--name <name>                     Display name shown to phones (default: 1PhoneMirror)
--width <px>                      Window width (default: 1280)
--height <px>                     Window height (default: 720)
--no-airplay                      Disable the AirPlay receiver
--no-miracast                     Disable the Miracast receiver
--no-android                      Disable the Android (scrcpy) receiver
--android-pair <ip:port> <code>   One-shot: pair with Wireless debugging then exit
--android-connect <ip:port>       One-shot: connect to a paired device then exit
--android-device <serial>         Auto-start streaming this device on launch
--android-jar <path>              Override path to scrcpy-server.jar
--android-adb <path>              Override path to adb.exe
```

## Packaging (MSI installer)

```powershell
.\package.ps1   # uses WiX Toolset 4 — produces dist\1PhoneMirror-<ver>.msi
```

The version string is auto-derived from `src/media/renderer.cpp` and bumped
in lockstep with `CMakeLists.txt`.

## Project status

| Component | State |
|-----------|-------|
| AirPlay 2 — RTSP + mDNS + FairPlay + multi-source | Working |
| AirPlay PIN pairing | Working |
| Android — ADB pair/connect + scrcpy v3 stream | Working |
| Miracast — WinRT receiver | Experimental |
| Hardware-accelerated decode (DXVA2 / D3D11VA) | Planned |
| System tray + autostart | Planned |
| Touch input forwarding (Miracast UIBC) | Planned |

## References

| Resource | What it provides |
|----------|------------------|
| [UxPlay](https://github.com/antimof/UxPlay) | AirPlay 2 receiver — FairPlay crypto, stream handling |
| [RPiPlay](https://github.com/FD-/RPiPlay) | Simpler AirPlay receiver — good FairPlay reference |
| [scrcpy](https://github.com/Genymobile/scrcpy) | The server-side JAR pushed to Android devices |
| [Windows.Media.Miracast](https://learn.microsoft.com/en-us/uwp/api/windows.media.miracast) | WinRT Miracast API docs |
| [Bonjour SDK](https://developer.apple.com/bonjour/) | DNS-SD for AirPlay discovery on Windows |

## License

1PhoneMirror is licensed under **GPL-3.0** — see [LICENSE](LICENSE).

### Third-party components

The Windows installer bundles the following components, each under its own
license:

| Component | Version | License | Role |
|-----------|---------|---------|------|
| [FFmpeg](https://ffmpeg.org/) | vcpkg LGPL build | LGPL-2.1+ | H.264 / AAC decoding (dynamic link) |
| [SDL2](https://www.libsdl.org/) | vcpkg | zlib | Window, input, rendering |
| [OpenSSL](https://www.openssl.org/) | 3.x | Apache-2.0 | TLS for AirPlay handshake |
| [scrcpy-server.jar](https://github.com/Genymobile/scrcpy) | 3.0 | Apache-2.0 | Pushed to Android device, runs there |
| [adb.exe](https://developer.android.com/tools/adb) | Android Platform Tools | Apache-2.0 + AOSP | Pair / connect to Android over Wi-Fi |
| [stb_image](https://github.com/nothings/stb) | header-only | MIT / Public Domain | PNG / JPG decode for the app icon |
| Microsoft VC++ Runtime | 14.x | Microsoft Redistributable License | C/C++ runtime DLLs |

All components are GPL-3.0 compatible. FFmpeg is built without `--enable-gpl`
or `--enable-nonfree` and linked dynamically, satisfying the LGPL.

