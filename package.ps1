<#
.SYNOPSIS
    Build, stage, and package 1PhoneMirror into an MSI (and optionally an
    Intune .intunewin) ready for distribution.

.DESCRIPTION
    Pipeline:
        1. Build Release with CMake/MSBuild (skippable with -SkipBuild).
        2. Stage 1PhoneMirror.exe + all required runtime DLLs and assets
           into  dist\stage\.
        3. Run WiX 5 to produce  dist\1PhoneMirror-<ver>.msi.
        4. (Optional) sign the MSI with signtool if -SignCertThumbprint given.
        5. (Optional) wrap into .intunewin if IntuneWinAppUtil.exe is on PATH
           (or its path is provided via -IntuneWinAppUtil).

.PARAMETER Version
    Version string written into the MSI (e.g. 0.3.0). Defaults to the version
    parsed from src\media\renderer.cpp footer (v0.x.y).

.PARAMETER SkipBuild
    Skip the CMake/MSBuild step (assumes build\Release is up to date).

.PARAMETER SignCertThumbprint
    SHA-1 thumbprint of a code-signing cert in CurrentUser\My. When provided,
    both the EXE (in stage) and the resulting MSI are signed with signtool.

.PARAMETER TimestampUrl
    RFC 3161 timestamp URL used during signing.

.PARAMETER IntuneWinAppUtil
    Optional path to IntuneWinAppUtil.exe. If found (or found on PATH), the
    MSI is wrapped into a .intunewin in dist\.

.EXAMPLE
    .\package.ps1
    Build + stage + MSI.

.EXAMPLE
    .\package.ps1 -SignCertThumbprint ABCDEF1234... -IntuneWinAppUtil C:\Tools\IntuneWinAppUtil.exe
    Full pipeline including signing and Intune packaging.
#>
[CmdletBinding()]
param(
    [string] $Version,
    [switch] $SkipBuild,
    [string] $SignCertThumbprint,
    [string] $TimestampUrl = 'http://timestamp.digicert.com',
    [string] $IntuneWinAppUtil
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

# ---------- 1. Determine version ----------
if (-not $Version) {
    $cmake = Join-Path $root 'CMakeLists.txt'
    if (Test-Path $cmake) {
        $m = Select-String -Path $cmake -Pattern 'project\s*\([^)]*VERSION\s+(\d+\.\d+\.\d+)' |
             Select-Object -First 1
        if ($m) { $Version = $m.Matches[0].Groups[1].Value }
    }
    if (-not $Version) {
        $rendererCpp = Join-Path $root 'src\media\renderer.cpp'
        if (Test-Path $rendererCpp) {
            # Match the info-panel title line specifically, not stray v0.x.y in comments.
            $m = Select-String -Path $rendererCpp -Pattern '1PhoneMirror v(\d+\.\d+\.\d+)' |
                 Select-Object -First 1
            if ($m) { $Version = $m.Matches[0].Groups[1].Value }
        }
    }
    if (-not $Version) { $Version = '0.0.0' }
}
Write-Host "==> Packaging 1PhoneMirror v$Version" -ForegroundColor Cyan

$buildDir = Join-Path $root 'build'
$releaseDir = Join-Path $buildDir 'Release'
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist 'stage'
$installerDir = Join-Path $root 'installer'

# ---------- 2. Build (Release) ----------
if (-not $SkipBuild) {
    Write-Host "==> Building Release configuration" -ForegroundColor Cyan
    & (Join-Path $root 'scripts\build.ps1')
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }
}

if (-not (Test-Path (Join-Path $releaseDir '1PhoneMirror.exe'))) {
    throw "1PhoneMirror.exe not found in $releaseDir. Run build first."
}

# ---------- 3. Stage ----------
Write-Host "==> Staging files in $stage" -ForegroundColor Cyan
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Path $stage | Out-Null

# Copy everything from build\Release (exe + runtime DLLs + assets that the
# build step already deployed via vcpkg's applocal.ps1).
Copy-Item -Path (Join-Path $releaseDir '*') -Destination $stage -Recurse -Force

# Add the third-party licenses next to the EXE.
Copy-Item (Join-Path $installerDir 'THIRD_PARTY_LICENSES.txt') $stage -Force

# ---------- 3b. VC++ runtime DLLs (vcruntime140.dll / msvcp140.dll) ----------
# These are not redistributed by vcpkg's applocal, so the EXE will fail on
# machines that don't already have the VC redist installed. Pull them from
# the MSVC Redist folder of the toolchain we built with.
function Find-VCRedistDir {
    $vsRoots = @(
        "${env:ProgramFiles}\Microsoft Visual Studio",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio"
    )
    foreach ($root in $vsRoots) {
        if (-not (Test-Path $root)) { continue }
        $candidates = Get-ChildItem -Path $root -Directory -Recurse -ErrorAction SilentlyContinue `
            -Filter 'Microsoft.VC*.CRT' |
            Where-Object { $_.FullName -match '\\Redist\\MSVC\\[\d.]+\\x64\\' }
        if ($candidates) {
            return ($candidates | Sort-Object FullName -Descending | Select-Object -First 1).FullName
        }
    }
    return $null
}

$vcDir = Find-VCRedistDir
if (-not $vcDir) {
    Write-Warning "VC++ redist folder not found - vcruntime140.dll / msvcp140.dll will be MISSING from the MSI."
} else {
    Write-Host "    Bundling VC++ runtime from $vcDir" -ForegroundColor DarkGray
    foreach ($dll in @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll')) {
        $src = Join-Path $vcDir $dll
        if (Test-Path $src) { Copy-Item $src $stage -Force }
    }
}

# Sanity check — required runtimes must be present.
$required = @('1PhoneMirror.exe', 'SDL2.dll', 'vcruntime140.dll', 'msvcp140.dll')
foreach ($f in $required) {
    if (-not (Test-Path (Join-Path $stage $f))) {
        throw "Required file missing from stage: $f"
    }
}
$ffmpeg = Get-ChildItem $stage -Filter 'av*.dll' | Measure-Object
if ($ffmpeg.Count -lt 3) {
    Write-Warning "Fewer than 3 FFmpeg DLLs in stage — check vcpkg deployment."
}

# ---------- 4. Optional: sign the EXE before packaging ----------
function Invoke-Signtool([string]$file) {
    if (-not $SignCertThumbprint) { return }
    $signtool = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if (-not $signtool) {
        Write-Warning "signtool.exe not found on PATH — skipping signing of $file"
        return
    }
    Write-Host "    signing $file" -ForegroundColor DarkGray
    & $signtool.Source sign /sha1 $SignCertThumbprint /fd SHA256 `
        /tr $TimestampUrl /td SHA256 $file
    if ($LASTEXITCODE -ne 0) { throw "signtool failed for $file" }
}

if ($SignCertThumbprint) {
    Write-Host "==> Signing staged binaries" -ForegroundColor Cyan
    Get-ChildItem $stage -Recurse -Include *.exe, *.dll | ForEach-Object {
        Invoke-Signtool $_.FullName
    }
}

# ---------- 5. Locate / install WiX ----------
Write-Host "==> Locating WiX 5 toolset" -ForegroundColor Cyan

# Make sure the global dotnet-tools directory is on PATH for this session.
$toolsDir = Join-Path $env:USERPROFILE '.dotnet\tools'
if ($env:PATH -notlike "*$toolsDir*") { $env:PATH = "$toolsDir;$env:PATH" }

$wix = Get-Command wix.exe -ErrorAction SilentlyContinue
if (-not $wix) {
    Write-Host "    wix.exe not found - installing as a global dotnet tool"

    # Resolve dotnet.exe even when it is not on PATH (common with new SDK installs
    # where the shell session was started before the installer ran).
    $dotnet = Get-Command dotnet.exe -ErrorAction SilentlyContinue
    if (-not $dotnet) {
        $candidates = @(
            "$env:ProgramFiles\dotnet\dotnet.exe",
            "$env:ProgramW6432\dotnet\dotnet.exe",
            "${env:ProgramFiles(x86)}\dotnet\dotnet.exe",
            "$env:LOCALAPPDATA\Microsoft\dotnet\dotnet.exe"
        ) | Where-Object { $_ -and (Test-Path $_) }
        if ($candidates) {
            $dotnetExe = $candidates[0]
            $dotnetDir = Split-Path -Parent $dotnetExe
            $env:PATH = "$dotnetDir;$env:PATH"
            $dotnet = Get-Command dotnet.exe -ErrorAction SilentlyContinue
        }
    }
    if (-not $dotnet) {
        throw "dotnet.exe not found. Install the .NET SDK from https://dot.net or open a new shell after installing it, then rerun .\package.ps1."
    }

    & $dotnet.Source tool install --global wix --version 5.* 2>&1 | Out-Null
    $wix = Get-Command wix.exe -ErrorAction SilentlyContinue
    if (-not $wix) {
        # Most common cause: no nuget.org source configured for the user.
        $sources = & $dotnet.Source nuget list source 2>&1
        if ($sources -notmatch 'nuget\.org') {
            Write-Host "    Adding nuget.org source and retrying..."
            & $dotnet.Source nuget add source https://api.nuget.org/v3/index.json -n nuget.org 2>&1 | Out-Null
            & $dotnet.Source tool install --global wix --version 5.* 2>&1 | Out-Null
            $wix = Get-Command wix.exe -ErrorAction SilentlyContinue
        }
    }
    if (-not $wix) { throw "Failed to install WiX 5. Run 'dotnet tool install --global wix --version 5.*' manually and rerun." }
}

# Ensure the firewall and UI extensions are installed (idempotent).
# Pin to the same major version as the wix tool itself (v5) — otherwise
# `wix extension add -g <name>` defaults to v7 and the build fails with
# "Could not find expected package root folder wixext5".
$wixVersion = (& $wix.Source --version) -replace '[^\d.].*$',''
$wixMinor = ($wixVersion -split '\.')[0..1] -join '.'
$extVersion = if ($wixMinor) { "$wixMinor.*" } else { '5.*' }
foreach ($ext in @('WixToolset.Firewall.wixext', 'WixToolset.UI.wixext')) {
    & $wix.Source extension add -g "$ext/$extVersion" 2>&1 | Out-Null
}

# ---------- 6. Build the MSI ----------
if (-not (Test-Path $dist)) { New-Item -ItemType Directory -Path $dist | Out-Null }
$msi = Join-Path $dist "1PhoneMirror-$Version.msi"
Write-Host "==> Building MSI: $msi" -ForegroundColor Cyan

$wxs = Join-Path $installerDir '1PhoneMirror.wxs'

& $wix.Source build $wxs `
    -ext WixToolset.Firewall.wixext `
    -ext WixToolset.UI.wixext `
    -arch x64 `
    -d "Version=$Version" `
    -d "StagingDir=$stage" `
    -bindpath $stage `
    -o $msi
if ($LASTEXITCODE -ne 0) { throw "wix build failed (exit $LASTEXITCODE)" }

# Sign the MSI itself.
Invoke-Signtool $msi

Write-Host ""
Write-Host "==> MSI ready: $msi" -ForegroundColor Green

# ---------- 7. Optional: wrap into .intunewin ----------
$intuneTool = $null
if ($IntuneWinAppUtil) {
    if (Test-Path $IntuneWinAppUtil) { $intuneTool = $IntuneWinAppUtil }
} else {
    $cmd = Get-Command IntuneWinAppUtil.exe -ErrorAction SilentlyContinue
    if ($cmd) { $intuneTool = $cmd.Source }
}

if ($intuneTool) {
    Write-Host "==> Wrapping into .intunewin via $intuneTool" -ForegroundColor Cyan
    $intuneOut = Join-Path $dist 'intune'
    if (-not (Test-Path $intuneOut)) { New-Item -ItemType Directory -Path $intuneOut | Out-Null }

    # IntuneWinAppUtil wants a source folder, not a single file. Copy MSI
    # alone into a clean folder so the .intunewin contains only the installer.
    $intuneSrc = Join-Path $dist 'intune-src'
    if (Test-Path $intuneSrc) { Remove-Item -Recurse -Force $intuneSrc }
    New-Item -ItemType Directory -Path $intuneSrc | Out-Null
    Copy-Item $msi $intuneSrc

    & $intuneTool -c $intuneSrc -s (Split-Path -Leaf $msi) -o $intuneOut -q
    if ($LASTEXITCODE -ne 0) { throw "IntuneWinAppUtil failed" }

    Write-Host "==> Intune package ready in: $intuneOut" -ForegroundColor Green
    Write-Host ""
    Write-Host "    Intune install command:   msiexec /i `"1PhoneMirror-$Version.msi`" /qn /l*v `"%ProgramData%\1PhoneMirror_install.log`""
    Write-Host "    Intune uninstall command: msiexec /x {ProductCode-from-MSI} /qn"
    Write-Host "    Detection rule:           MSI product code (auto)"
    Write-Host "    Install behavior:         System"
} else {
    Write-Host ""
    Write-Host "(IntuneWinAppUtil.exe not found — skipping .intunewin wrap.)" -ForegroundColor DarkGray
    Write-Host "Download from: https://github.com/Microsoft/Microsoft-Win32-Content-Prep-Tool"
}

Write-Host ""
Write-Host "==> Done." -ForegroundColor Green
