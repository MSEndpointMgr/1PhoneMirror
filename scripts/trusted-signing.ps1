<#
.SYNOPSIS
    Azure Trusted Signing (Artifact Signing) helper for 1PhoneMirror.

.DESCRIPTION
    Signs files with the cloud-hosted certificate profile via signtool's
    /dlib provider. No certificate ever lands in the local store — the private
    key stays in Azure and signing is authorized by your Azure login plus the
    "Trusted Signing Certificate Profile Signer" role.

    Exposes:
      Get-TrustedSigningDlib   - ensures Azure.CodeSigning.Dlib.dll is present
                                 (downloads the NuGet package on first use),
                                 returns its full path.
      Resolve-Signtool         - locates the newest x64 signtool.exe.
      New-TrustedSigningMetadata - writes the account/profile metadata JSON.
      Invoke-TrustedSign       - signs one file.

    Dot-source this file, then call the functions.
#>

$script:TsRoot = Join-Path $env:LOCALAPPDATA '1PhoneMirror\trusted-signing'
$script:TsPackage = 'Microsoft.Trusted.Signing.Client'

function Get-TrustedSigningDlib {
    [CmdletBinding()]
    param(
        [string] $Version  # optional pin; latest stable if omitted
    )

    $existing = Get-ChildItem -Path $script:TsRoot -Recurse -Filter 'Azure.CodeSigning.Dlib.dll' `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($existing) { return $existing.FullName }

    New-Item -ItemType Directory -Force -Path $script:TsRoot | Out-Null

    $pkgLower = $script:TsPackage.ToLowerInvariant()
    if (-not $Version) {
        $idxUrl = "https://api.nuget.org/v3-flatcontainer/$pkgLower/index.json"
        $idx = Invoke-RestMethod -Uri $idxUrl -UseBasicParsing
        $Version = ($idx.versions | Where-Object { $_ -notmatch '-' } | Select-Object -Last 1)
        if (-not $Version) { $Version = $idx.versions | Select-Object -Last 1 }
    }

    $nupkgUrl = "https://api.nuget.org/v3-flatcontainer/$pkgLower/$Version/$pkgLower.$Version.nupkg"
    $nupkg = Join-Path $script:TsRoot "$pkgLower.$Version.zip"
    Write-Host "    Downloading $script:TsPackage $Version" -ForegroundColor DarkGray
    Invoke-WebRequest -Uri $nupkgUrl -OutFile $nupkg -UseBasicParsing

    $extractDir = Join-Path $script:TsRoot $Version
    if (Test-Path $extractDir) { Remove-Item -Recurse -Force $extractDir }
    Expand-Archive -Path $nupkg -DestinationPath $extractDir -Force
    Remove-Item $nupkg -Force

    $dlib = Get-ChildItem -Path $extractDir -Recurse -Filter 'Azure.CodeSigning.Dlib.dll' `
        -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\x64\\' } |
        Select-Object -First 1
    if (-not $dlib) {
        $dlib = Get-ChildItem -Path $extractDir -Recurse -Filter 'Azure.CodeSigning.Dlib.dll' `
            -ErrorAction SilentlyContinue | Select-Object -First 1
    }
    if (-not $dlib) { throw "Azure.CodeSigning.Dlib.dll not found inside $script:TsPackage $Version." }
    return $dlib.FullName
}

function Resolve-Signtool {
    [CmdletBinding()]
    param()

    $cmd = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $kits = 'C:\Program Files (x86)\Windows Kits\10\bin'
    if (Test-Path $kits) {
        $st = Get-ChildItem -Path $kits -Recurse -Filter 'signtool.exe' -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\' } |
            Sort-Object { [version]($_.Directory.Parent.Name) } -Descending |
            Select-Object -First 1
        if ($st) { return $st.FullName }
    }
    throw 'signtool.exe not found. Install the Windows 10/11 SDK (Signing Tools).'
}

function New-TrustedSigningMetadata {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $Endpoint,
        [Parameter(Mandatory)] [string] $AccountName,
        [Parameter(Mandatory)] [string] $ProfileName,
        [string] $Path
    )
    if (-not $Path) { $Path = Join-Path $script:TsRoot 'metadata.json' }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    $meta = [ordered]@{
        Endpoint               = $Endpoint
        CodeSigningAccountName = $AccountName
        CertificateProfileName = $ProfileName
    }
    $meta | ConvertTo-Json | Set-Content -Path $Path -Encoding ASCII
    return $Path
}

function Invoke-TrustedSign {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $File,
        [Parameter(Mandatory)] [string] $Signtool,
        [Parameter(Mandatory)] [string] $Dlib,
        [Parameter(Mandatory)] [string] $MetadataPath,
        [string] $TimestampUrl = 'http://timestamp.acs.microsoft.com'
    )
    Write-Host "    signing $([IO.Path]::GetFileName($File))" -ForegroundColor DarkGray
    & $Signtool sign /v /fd SHA256 /tr $TimestampUrl /td SHA256 `
        /dlib $Dlib /dmdf $MetadataPath $File
    if ($LASTEXITCODE -ne 0) { throw "Trusted Signing failed for $File (signtool exit $LASTEXITCODE)." }
}
