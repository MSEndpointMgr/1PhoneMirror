# 1PhoneMirror Windows Dependency Setup
# Run this script to install all build dependencies via vcpkg

param(
    [string]$VcpkgRoot = "$env:USERPROFILE\vcpkg",
    [switch]$SkipVcpkg,
    [switch]$SkipBonjour
)

$ErrorActionPreference = "Stop"

Write-Host "=== 1PhoneMirror Dependency Setup ===" -ForegroundColor Cyan

# --- 1. Install vcpkg if not present ---
if (-not $SkipVcpkg) {
    if (-not (Test-Path "$VcpkgRoot\vcpkg.exe")) {
        Write-Host "`n[1/4] Installing vcpkg..." -ForegroundColor Yellow
        git clone https://github.com/microsoft/vcpkg.git $VcpkgRoot
        & "$VcpkgRoot\bootstrap-vcpkg.bat" -disableMetrics
    } else {
        Write-Host "`n[1/4] vcpkg already installed at $VcpkgRoot" -ForegroundColor Green
    }

    # Integrate vcpkg with MSBuild / CMake
    & "$VcpkgRoot\vcpkg.exe" integrate install
}

# --- 2. Install C++ dependencies ---
Write-Host "`n[2/4] Installing C++ libraries via vcpkg..." -ForegroundColor Yellow

$packages = @(
    "ffmpeg[avcodec,avformat,avfilter,swscale,swresample,gpl,x264]:x64-windows",
    "sdl2:x64-windows",
    "openssl:x64-windows"
)

foreach ($pkg in $packages) {
    Write-Host "  Installing $pkg..."
    & "$VcpkgRoot\vcpkg.exe" install $pkg
}

# --- 3. Bonjour SDK (for AirPlay mDNS) ---
if (-not $SkipBonjour) {
    Write-Host "`n[3/4] Checking Bonjour SDK..." -ForegroundColor Yellow

    $bonjourPaths = @(
        "${env:ProgramFiles}\Bonjour SDK",
        "${env:ProgramFiles(x86)}\Bonjour SDK",
        "$env:BONJOUR_SDK_HOME"
    )

    $bonjourFound = $false
    foreach ($path in $bonjourPaths) {
        if ($path -and (Test-Path "$path\Include\dns_sd.h")) {
            Write-Host "  Bonjour SDK found at: $path" -ForegroundColor Green
            [System.Environment]::SetEnvironmentVariable("BONJOUR_SDK_HOME", $path, "User")
            $bonjourFound = $true
            break
        }
    }

    if (-not $bonjourFound) {
        Write-Host @"
  Bonjour SDK not found. To enable AirPlay (iOS) support:
  1. Download from: https://developer.apple.com/bonjour/
  2. Install to default location
  3. Set BONJOUR_SDK_HOME environment variable
  4. Or re-run this script

  You can still build with Miracast (Android) support without it.
"@ -ForegroundColor DarkYellow
    }
}

# --- 4. Check Visual Studio / Build Tools ---
Write-Host "`n[4/4] Checking build tools..." -ForegroundColor Yellow

# Find VS installations via vswhere
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vsInstalls = & $vswhere -all -products * -format json | ConvertFrom-Json
    $vsWithCpp = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($vsWithCpp) {
        Write-Host "  Visual Studio with C++: $vsWithCpp" -ForegroundColor Green
    } else {
        Write-Host "  Visual Studio found but C++ workload missing!" -ForegroundColor Red
        Write-Host "  Run: winget install Microsoft.VisualStudio.2022.BuildTools --override `"--wait --quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended`"" -ForegroundColor DarkYellow
    }
} else {
    Write-Host "  Visual Studio not found!" -ForegroundColor Red
    Write-Host "  Run: winget install Microsoft.VisualStudio.2022.BuildTools --override `"--wait --quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended`"" -ForegroundColor DarkYellow
}

# Check for CMake (prefer VS-bundled, then PATH)
$cmakePath = $null
if ($vsWithCpp) {
    $vsCmake = Get-ChildItem "$vsWithCpp\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -ErrorAction SilentlyContinue
    if ($vsCmake) { $cmakePath = $vsCmake.FullName }
}
if (-not $cmakePath) {
    $cmakePath = Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
}
if ($cmakePath) {
    $cmakeVersion = & $cmakePath --version 2>$null | Select-Object -First 1
    Write-Host "  CMake: $cmakeVersion ($cmakePath)" -ForegroundColor Green
} else {
    Write-Host "  CMake not found! It's included with VS Build Tools C++ workload." -ForegroundColor Red
}

# --- Done ---
Write-Host "`n=== Setup Complete ===" -ForegroundColor Cyan
Write-Host @"

To build 1PhoneMirror:

  mkdir build && cd build
  cmake .. -DCMAKE_TOOLCHAIN_FILE="$VcpkgRoot/scripts/buildsystems/vcpkg.cmake"
  cmake --build . --config Release

To run:

  .\Release\1phonemirror.exe --name "My PC"

"@
