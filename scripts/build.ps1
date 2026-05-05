# 1PhoneMirror Build Script

param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release",

    [string]$VcpkgRoot = "$env:USERPROFILE\vcpkg",

    [switch]$Clean,
    [switch]$NoAirPlay,
    [switch]$NoMiracast
)

$ErrorActionPreference = "Stop"
$buildDir = Join-Path $PSScriptRoot "..\build"

# --- Find cmake ---
$cmake = Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
if (-not $cmake) {
    # Look for VS-bundled CMake
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($vsPath) {
            $vsCmake = Get-ChildItem "$vsPath\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -ErrorAction SilentlyContinue
            if ($vsCmake) { $cmake = $vsCmake.FullName }
        }
    }
}
if (-not $cmake) {
    Write-Error "CMake not found. Install Visual Studio Build Tools with C++ workload."
    exit 1
}
Write-Host "Using CMake: $cmake" -ForegroundColor Gray

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Cleaning build directory..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}

if (-not (Test-Path $buildDir)) {
    New-Item -ItemType Directory -Path $buildDir | Out-Null
}

Push-Location $buildDir

try {
    $cmakeArgs = @(
        "..",
        "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake"
    )

    if ($NoAirPlay) { $cmakeArgs += "-DENABLE_AIRPLAY=OFF" }
    if ($NoMiracast) { $cmakeArgs += "-DENABLE_MIRACAST=OFF" }

    Write-Host "Configuring..." -ForegroundColor Cyan
    & $cmake @cmakeArgs

    Write-Host "`nBuilding ($Config)..." -ForegroundColor Cyan
    & $cmake --build . --config $Config --parallel

    Write-Host "`nBuild complete: build\$Config\1phonemirror.exe" -ForegroundColor Green
}
finally {
    Pop-Location
}
