# Getting Started with 1PhoneMirror

This guide walks you through setting up your development environment, building, and running 1PhoneMirror on Windows.

## Prerequisites

| Requirement | Minimum Version | Notes |
|---|---|---|
| Windows | 10 (1903+) | Miracast requires 1903 or later |
| Visual Studio 2022 | 17.x | Must include the **C++ Desktop** workload |
| CMake | 3.20+ | Bundled with VS Build Tools, or install separately |
| Git | Any recent | Needed to clone vcpkg |

### Installing Visual Studio Build Tools

If you don't have Visual Studio with C++ support, install it via `winget`:

```powershell
winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

This includes CMake, the MSVC compiler, and the Windows SDK.

## Step 1 — Install Dependencies

From the project root, run:

```powershell
.\scripts\setup_deps.ps1
```

This script will:

1. **Clone and bootstrap vcpkg** to `%USERPROFILE%\vcpkg` (skipped if already present)
2. **Install C++ libraries** via vcpkg:
   - `ffmpeg` (avcodec, avformat, swscale, swresample)
   - `sdl2`
   - `openssl`
3. **Check for Bonjour SDK** (needed for AirPlay/iOS support)
4. **Verify** Visual Studio and CMake are available

If the script fails, check the error output — the most common issues are:

- **Missing C++ workload**: Install Build Tools with the command above
- **Git not in PATH**: Needed for vcpkg clone
- **Network issues**: vcpkg downloads source packages during install

### Optional parameters

```powershell
# Use a custom vcpkg location
.\scripts\setup_deps.ps1 -VcpkgRoot "D:\tools\vcpkg"

# Skip vcpkg install (if you manage it separately)
.\scripts\setup_deps.ps1 -SkipVcpkg

# Skip Bonjour SDK check
.\scripts\setup_deps.ps1 -SkipBonjour
```

## Step 2 — Bonjour SDK (Optional — AirPlay only)

AirPlay discovery uses mDNS via Apple's Bonjour SDK. Without it, only Miracast (Android) will work.

1. Download from [developer.apple.com/bonjour](https://developer.apple.com/bonjour/)
2. Install to the default location
3. Set the environment variable:

```powershell
[System.Environment]::SetEnvironmentVariable("BONJOUR_SDK_HOME", "C:\Program Files\Bonjour SDK", "User")
```

4. Restart your terminal so the variable takes effect

## Step 3 — Build

The quickest way:

```powershell
.\scripts\build.ps1
```

### Build script options

| Parameter | Description |
|---|---|
| `-Config Debug` | Build in Debug mode (default: `Release`) |
| `-Clean` | Delete the `build/` directory before building |
| `-NoAirPlay` | Disable AirPlay module (skips Bonjour dependency) |
| `-NoMiracast` | Disable Miracast module |
| `-VcpkgRoot <path>` | Custom vcpkg location (default: `%USERPROFILE%\vcpkg`) |

Examples:

```powershell
# Debug build
.\scripts\build.ps1 -Config Debug

# Clean rebuild without AirPlay
.\scripts\build.ps1 -Clean -NoAirPlay
```

### Manual build (alternative)

```powershell
mkdir build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE="$env:USERPROFILE\vcpkg\scripts\buildsystems\vcpkg.cmake"
cmake --build . --config Release
```

## Step 4 — Run

```powershell
.\build\Release\1phonemirror.exe --name "My PC"
```

### Command line options

| Option | Default | Description |
|---|---|---|
| `--name <name>` | `1PhoneMirror` | Name shown on mobile devices during discovery |
| `--width <px>` | `1280` | Window width |
| `--height <px>` | `720` | Window height |
| `--no-airplay` | — | Disable AirPlay receiver |
| `--no-miracast` | — | Disable Miracast receiver |

### Keyboard shortcuts

| Key | Action |
|---|---|
| `F` | Toggle fullscreen |
| `W` | Toggle webcam drawer (right-click the `W` button to pick a camera) |
| `ESC` | Quit |

## Step 5 — Connect a Device

### iOS / macOS (AirPlay)

1. Make sure your device and PC are on the **same Wi-Fi network**
2. Open **Control Center** → tap **Screen Mirroring**
3. Select your PC's name from the list

### Android (Miracast)

1. Open **Settings** → **Connected Devices** → **Cast** (may be called "Smart View" or "Screen Mirror" on some devices)
2. Your PC will appear as a wireless display
3. Tap to connect

## Troubleshooting

### `setup_deps.ps1` fails with exit code 1

- Make sure you are running PowerShell as a regular user (not necessarily admin)
- Check that `git` is installed and on your PATH
- If vcpkg install times out, re-run the script — it will resume from where it left off

### CMake cannot find FFmpeg / SDL2

- Ensure you pass `-DCMAKE_TOOLCHAIN_FILE` pointing to vcpkg's toolchain file
- The build script handles this automatically — prefer `.\scripts\build.ps1`

### AirPlay device doesn't see the PC

- Verify Bonjour SDK is installed and `BONJOUR_SDK_HOME` is set
- Ensure both devices are on the same subnet (some routers isolate Wi-Fi clients)
- Check Windows Firewall — allow `1phonemirror.exe` through

### Miracast device doesn't see the PC

- Miracast requires Windows 10 1903+ and a compatible Wi-Fi adapter (Wi-Fi Direct support)
- Check that Wi-Fi Direct is enabled: `netsh wlan show drivers` — look for "Wireless Display Supported: Yes"

## Project Status

This is a **scaffold project**. The build system and media pipeline work, but several protocol-level features are incomplete — see the [README](README.md) for the full TODO list. The most significant gap is the **FairPlay handshake** needed for modern iOS AirPlay connections.
