# 1PhoneMirror

An open-source screen mirroring receiver for Windows that supports both **AirPlay** (iOS/macOS) and **Miracast** (Android) using native device casting — no apps needed on the mobile device.

## Features

- **AirPlay 2 Receiver** — iOS and macOS devices can mirror their screen via the native Screen Mirroring control
- **Miracast Receiver** — Android devices can cast via the built-in "Cast" / "Smart View" / "Screen Mirror" feature
- **Unified rendering** — Single SDL2 window with GPU-accelerated display
- **Low latency** — FFmpeg hardware-accelerated H.264/H.265 decoding
- **Fullscreen** — Press `F` to toggle, `ESC` to quit
- **Cross-protocol** — Both protocols feed into the same rendering pipeline

## Architecture

```
iOS (AirPlay)          Android (Miracast/Wi-Fi Display)
      │                          │
      ▼                          ▼
┌─────────────┐         ┌───────────────────┐
│ mDNS + RTSP │         │ WinRT Miracast API │
│ FairPlay    │         │ (Wi-Fi Direct)     │
└──────┬──────┘         └─────────┬─────────┘
       │                          │
       ▼                          ▼
┌──────────────────────────────────────┐
│     FFmpeg Decoder (H.264/H.265)     │
├──────────────────────────────────────┤
│     SDL2 Renderer + Audio Output     │
└──────────────────────────────────────┘
```

## Prerequisites

- **Windows 10/11** (Miracast requires Windows 10 1903+)
- **Visual Studio 2022** with C++ Desktop workload (or Build Tools)
- **CMake** 3.20+
- **vcpkg** (for dependency management)
- **Bonjour SDK** (for AirPlay mDNS — optional)

## Quick Start

### 1. Install Dependencies

```powershell
.\scripts\setup_deps.ps1
```

This installs via vcpkg:
- FFmpeg (libavcodec, libavformat, libswscale, libswresample)
- SDL2
- OpenSSL

### 2. Build

```powershell
.\scripts\build.ps1

# Or manually:
mkdir build; cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE="$env:USERPROFILE\vcpkg\scripts\buildsystems\vcpkg.cmake"
cmake --build . --config Release
```

### 3. Run

```powershell
.\build\Release\1phonemirror.exe --name "My PC"
```

### Command Line Options

| Option | Description |
|--------|-------------|
| `--name <name>` | Display name shown on mobile devices (default: 1PhoneMirror) |
| `--width <px>` | Window width (default: 1280) |
| `--height <px>` | Window height (default: 720) |
| `--no-airplay` | Disable AirPlay (iOS) receiver |
| `--no-miracast` | Disable Miracast (Android) receiver |

## Connecting Devices

### iOS / macOS (AirPlay)
1. Ensure your iPhone/iPad/Mac and PC are on the **same Wi-Fi network**
2. Open **Control Center** → tap **Screen Mirroring**
3. Select your PC's name from the list

### Android (Miracast)
1. Open **Settings** → **Connected Devices** → **Cast** (or use Quick Settings tile)
2. Your PC will appear as a wireless display target
3. Tap to connect

## Project Status

This is a **scaffold/kickstart project**. Here's what works and what needs work:

### Working
- [x] Project structure and CMake build system
- [x] FFmpeg video decoder pipeline (H.264 → RGBA)
- [x] SDL2 renderer with aspect-ratio preservation and fullscreen
- [x] Audio output via SDL2
- [x] TCP/RTSP server framework
- [x] AirPlay mDNS service advertisement
- [x] AirPlay RTSP control channel (session lifecycle)
- [x] AirPlay mirror data receiver (frame parsing)
- [x] Miracast receiver via WinRT API
- [x] Dependency setup and build scripts

### TODO — Critical Path
- [ ] **FairPlay handshake** — Port the `lib/playfair` crypto from [UxPlay](https://github.com/antimof/UxPlay). Without this, modern iOS devices won't complete the AirPlay connection. This is the single biggest piece of work.
- [ ] **Binary plist handling** — AirPlay `/info` and `/server-info` endpoints need proper binary plist responses. Use a plist library or port from UxPlay.
- [ ] **Stream decryption** — After FairPlay handshake, the mirror stream is AES-encrypted. Decryption keys come from the handshake.
- [ ] **Miracast frame extraction** — The WinRT `MiracastReceiver` API handles the protocol, but extracting raw video frames for custom rendering needs the `MediaFrameReader` integration.

### TODO — Nice to Have
- [ ] Hardware-accelerated decoding (DXVA2 / D3D11VA)
- [ ] System tray icon with auto-start
- [ ] Audio-only AirPlay (Spotify/music streaming to PC speakers)
- [ ] Multiple simultaneous connections
- [ ] PIN/password protection
- [ ] Touch input forwarding (Miracast UIBC)

## Key References

| Resource | What it provides |
|----------|-----------------|
| [UxPlay](https://github.com/antimof/UxPlay) | Complete AirPlay 2 receiver — FairPlay crypto, stream handling |
| [RPiPlay](https://github.com/FD-/RPiPlay) | Simpler AirPlay receiver — good FairPlay reference |
| [MiracleCast](https://github.com/nicman23/miraclecast) | Linux Miracast — protocol reference |
| [Windows.Media.Miracast](https://learn.microsoft.com/en-us/uwp/api/windows.media.miracast) | WinRT Miracast API docs |
| [Bonjour SDK](https://developer.apple.com/bonjour/) | DNS-SD for AirPlay discovery on Windows |

## License

MIT License — see [LICENSE](LICENSE).
