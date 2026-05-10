# 1PhoneMirror

An open-source screen-mirroring receiver for Windows that lets an iPhone or
an Android phone show up inside a phone-shaped window on your PC — no app
installed on the phone, no cables.

- **iOS / iPadOS / macOS** — native AirPlay (Screen Mirroring), with optional PIN pairing.
- **Android** — Wireless Debugging via the bundled `adb` + `scrcpy-server.jar`.
- **Miracast** — Wi-Fi Direct receiver (experimental, Windows-only).
- **Capture** — one-click screenshots (PNG) and screen recording (MP4 or GIF) straight into a phone-framed picture or clip — perfect for documentation.

Multiple phones can stay paired at once and appear as small dots in the
bottom bezel; left-click switches the active source, right-click opens a
menu to disconnect.

> Like the project? [Buy me a coffee ☕](https://buymeacoffee.com/simonskothn) — every tip helps keep development going.

## Why 1PhoneMirror exists

I build documentation and deliver training on managing Android and iOS
devices through Microsoft Intune. Every walkthrough needs clean
screenshots of the phone — ideally framed in a real device shape so
readers immediately know what they're looking at.

Finding a tool that did just that turned out to be surprisingly painful:
the free options either didn't work reliably, locked the good features
behind a subscription, or produced bare un-framed captures I had to
re-edit by hand. So I wrote my own. **1PhoneMirror** is the result —
a no-install, no-cable mirror that drops a properly-framed phone image
straight into your screenshots and recordings, ready to paste into a
guide, a Loop page, or a slide deck.
## Screenshots

| | |
|---|---|
| ![Waiting for a connection](docs/screenshots/01-waiting.png) | ![Info panel](docs/screenshots/02-info-panel.png) |
| **Idle screen** — quick reminders for AirPlay (iOS) and Wireless debugging (Android), with the framed phone window ready to receive. | **Info panel** (`I`) — version, shortcuts, network requirements, and a one-click "Copy network test script" for IT validation. |
| ![Settings panel](docs/screenshots/03-settings-panel.png) | ![Log viewer](docs/screenshots/04-log-viewer.png) |
| **Settings panel** (`S`) — bezel colour swatches, screenshot/clipboard toggles, computer-name identity, and the MP4 / GIF recording-format selector. | **Log viewer** (`L`) — live activity drawer slides out to the right, perfect for debugging AirPlay handshakes or Android pairing. |
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
                       │   + audio output         │  screenshots / MP4 /
                       │   + recorder (libx264)   │  GIF, source dots
                       └──────────────────────────┘
```

Source layout:

| Path | Contents |
|------|----------|
| `src/airplay/` | mDNS service, RTSP server, AirPlay mirror session, FairPlay |
| `src/android/` | `AdbController` (Win32 process pipes), scrcpy v3 stream receiver, in-app pair/connect dialog |
| `src/miracast/` | WinRT Miracast receiver |
| `src/media/` | FFmpeg decoder, SDL2 renderer, audio output, phone-frame overlay, screen recorder (MP4/GIF) |
| `src/network/` | TCP / RTSP plumbing |
| `src/app.cpp` | Wires sources to renderer, owns lifecycle |
| `tools/adb/` | Bundled platform-tools `adb.exe` (staged at build time) |
| `tools/scrcpy-server.jar` | scrcpy v3.0 server, pushed to the phone over ADB |

## Install

The fastest way (Windows 10 / 11):

```powershell
winget install MSEndpointMgr.1PhoneMirror
```

Or grab the latest MSI directly from
[Releases](https://github.com/MSEndpointMgr/1PhoneMirror/releases/latest).

## Build from source

### Prerequisites

- Windows 10 1903+ (Miracast) / Windows 11 recommended
- Visual Studio 2022 with the C++ Desktop workload (or the Build Tools)
- CMake 3.20+
- vcpkg (the build script uses `$env:VCPKG_ROOT` or `%USERPROFILE%\vcpkg`)

### Steps

```powershell
.\scripts\setup_deps.ps1   # one-time vcpkg install of FFmpeg (with libx264), SDL2, OpenSSL
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

## Capture

| Output | How |
|--------|-----|
| **Screenshot (PNG)** | `Ctrl+S`, or click the white circle in the menu/bezel. Saved to `Pictures\1PhoneMirror\` and/or copied to clipboard (toggle in Settings). |
| **Video (MP4)** | `Ctrl+R`, or click the red circle in the menu/bezel. H.264 via libx264, 30 fps by default. |
| **Animated GIF** | Switch format in Settings → record as above. Auto-downscaled to 480 px wide. |
| **Delayed recording** | Right-click the record button → **Start in 5 s**. Frosty countdown overlays the screen. |
| **Timed clip** | Right-click → **Record 5 s / 10 s / 15 s** — auto-stops at the chosen duration. |

All output lands in your `Pictures\1PhoneMirror\` folder; click the folder
button on the menu (or use the Windows file dialog) to open it.

## Keyboard shortcuts

| Key | Action |
|-----|--------|
| `F` | Toggle fullscreen |
| `M` | Toggle island menu |
| `L` | Toggle log viewer |
| `A` | Open Android pair / connect dialog |
| `I` | Toggle info panel |
| `V` | Toggle version history |
| `S` | Toggle settings panel (bezel colour, screenshots, recording format) |
| `Ctrl+S` | Screenshot — save to Pictures folder and/or clipboard (per Settings) |
| `Ctrl+R` | Start / stop screen recording (MP4 or GIF, per Settings) |
| `Ctrl+C` | (Log viewer) Copy entire log to clipboard |
| `Ctrl+X` | (Log viewer) Clear log |
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
.\package.ps1   # uses WiX Toolset 5 — produces dist\1PhoneMirror-<ver>.msi
```

The version string is sourced from `CMakeLists.txt` and propagated to the
binary, the in-app footer, and the MSI metadata.

## Version history

| Version | Date | Highlights |
|---------|------|------------|
| **0.3.0** | 10.05.2026 | **Screen recording** — MP4 (H.264) and GIF, `Ctrl+R`, right-click for 5 s delay or 5 / 10 / 15 s timed clips. Frosty countdown overlay, REC chip while recording. Format selector in Settings. New bezel record button alongside the screenshot button. |
| **0.2.5** | 09.05.2026 | Info panel: copy network-test PowerShell script (with MDM check). |
| **0.2.4** | 08.05.2026 | Refined bezel toggles and auto-collapse on connect. |
| **0.2.3** | 08.05.2026 | Better Android experience and quicker screenshots. |
| **0.2.2** | 08.05.2026 | Settings: identify as computer name on the network. Bezel device dots show play/pause icon per source. |
| **0.2.1** | 07.05.2026 | Settings panel (`S`): bezel colour + screenshot options. |
| **0.2.0** | 06.05.2026 | Android mirroring (Wireless debugging, press `A`). |
| **0.1.6** | 06.05.2026 | Multiple iOS devices stay paired — switch from bezel dots. |
| **0.1.5** | 06.05.2026 | AirPlay PIN pairing for trusted-device security. |

The full version log is also available in-app — press `V`.

## Project status

| Component | State |
|-----------|-------|
| AirPlay 2 — RTSP + mDNS + FairPlay + multi-source | Working |
| AirPlay PIN pairing | Working |
| Android — ADB pair/connect + scrcpy v3 stream | Working |
| Screenshots (PNG) | Working |
| Screen recording (MP4 H.264, GIF) | Working |
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

## Support the project

If 1PhoneMirror saves you time on Intune docs, training, or just casual
demos, consider tipping a coffee — it directly funds new features and
keeps the project free for everyone.

[![Buy me a coffee](https://img.shields.io/badge/Buy%20me%20a%20coffee-FFDD00?style=for-the-badge&logo=buymeacoffee&logoColor=black)](https://buymeacoffee.com/simonskothn)

## License

1PhoneMirror is licensed under **GPL-3.0** — see [LICENSE](LICENSE).

### Third-party components

The Windows installer bundles the following components, each under its own
license:

| Component | Version | License | Role |
|-----------|---------|---------|------|
| [FFmpeg](https://ffmpeg.org/) | vcpkg `gpl,x264` build | GPL-2.0+ (compatible with GPL-3.0) | H.264 / AAC decoding + recording (dynamic link) |
| [libx264](https://www.videolan.org/developers/x264.html) | bundled with FFmpeg | GPL-2.0+ | H.264 encoder for MP4 recording |
| [SDL2](https://www.libsdl.org/) | vcpkg | zlib | Window, input, rendering |
| [OpenSSL](https://www.openssl.org/) | 3.x | Apache-2.0 | TLS for AirPlay handshake |
| [scrcpy-server.jar](https://github.com/Genymobile/scrcpy) | 3.0 | Apache-2.0 | Pushed to Android device, runs there |
| [adb.exe](https://developer.android.com/tools/adb) | Android Platform Tools | Apache-2.0 + AOSP | Pair / connect to Android over Wi-Fi |
| [stb_image](https://github.com/nothings/stb) | header-only | MIT / Public Domain | PNG / JPG decode for the app icon |
| Microsoft VC++ Runtime | 14.x | Microsoft Redistributable License | C/C++ runtime DLLs |

All components are GPL-3.0 compatible.

