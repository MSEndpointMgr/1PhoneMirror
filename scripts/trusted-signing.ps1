<#
.SYNOPSIS
    Azure Trusted Signing (Artifact Signing) helper for 1PhoneMirror.

.DESCRIPTION
    Signs files with the cloud-hosted certificate profile via signtool's
    /dlib provider. No certificate ever lands in the local store — the private
    key stays in Azure and signing is authorized by your Azure login plus the
    "Trusted Signing Certificate Profile Signer" role.

    Exposes:
      Get-TrustedSigningDlib   - ensures the pinned, SHA-256-verified
                                 Azure.CodeSigning.Dlib.dll is present and
                                 returns its full path.
      Resolve-Signtool         - locates the newest x64 signtool.exe.
      New-TrustedSigningMetadata - writes the account/profile metadata JSON.
      Invoke-TrustedSign       - signs one file.

    Dot-source this file, then call the functions.
#>

$script:TsRoot = Join-Path $env:LOCALAPPDATA '1PhoneMirror\trusted-signing'
$script:TsPackage = 'Microsoft.Trusted.Signing.Client'
$script:TsVersion = '1.0.95'
$script:TsPackageSha256 = '3BFCF1E0A3CB42AF1692F0A8ED45C15DE070C2DE86F28A59B2795D904D8A920F'
$script:TsDlibSha256 = 'A359B420F676BC0223A379A84CA8369588AE7F265FD4F3E761E3425CBA376916'

function Get-TrustedSigningDlib {
    [CmdletBinding()]
    param()

    $extractDir = Join-Path $script:TsRoot $script:TsVersion
    $dlib = Join-Path $extractDir 'bin\x64\Azure.CodeSigning.Dlib.dll'
    if (Test-Path $dlib) {
        $actualDlibHash = (Get-FileHash -Path $dlib -Algorithm SHA256).Hash
        if ($actualDlibHash -eq $script:TsDlibSha256) { return $dlib }

        Write-Warning "Cached Azure.CodeSigning.Dlib.dll failed SHA-256 verification; downloading a clean copy."
        Remove-Item -Path $extractDir -Recurse -Force
    }

    New-Item -ItemType Directory -Force -Path $script:TsRoot | Out-Null

    $pkgLower = $script:TsPackage.ToLowerInvariant()
    $nupkgUrl = "https://api.nuget.org/v3-flatcontainer/$pkgLower/$script:TsVersion/$pkgLower.$script:TsVersion.nupkg"
    $nupkg = Join-Path $script:TsRoot "$pkgLower.$script:TsVersion.nupkg"
    try {
        Write-Host "    Downloading pinned $script:TsPackage $script:TsVersion" -ForegroundColor DarkGray
        Invoke-WebRequest -Uri $nupkgUrl -OutFile $nupkg -UseBasicParsing

        $actualPackageHash = (Get-FileHash -Path $nupkg -Algorithm SHA256).Hash
        if ($actualPackageHash -ne $script:TsPackageSha256) {
            throw "SHA-256 verification failed for $script:TsPackage $script:TsVersion (expected $script:TsPackageSha256, got $actualPackageHash)."
        }

        if (Test-Path $extractDir) { Remove-Item -Path $extractDir -Recurse -Force }
        Expand-Archive -Path $nupkg -DestinationPath $extractDir -Force
    }
    finally {
        if (Test-Path $nupkg) { Remove-Item -Path $nupkg -Force }
    }

    if (-not (Test-Path $dlib)) {
        throw "x64 Azure.CodeSigning.Dlib.dll not found inside $script:TsPackage $script:TsVersion."
    }
    $actualDlibHash = (Get-FileHash -Path $dlib -Algorithm SHA256).Hash
    if ($actualDlibHash -ne $script:TsDlibSha256) {
        Remove-Item -Path $extractDir -Recurse -Force
        throw "SHA-256 verification failed for Azure.CodeSigning.Dlib.dll (expected $script:TsDlibSha256, got $actualDlibHash)."
    }
    return $dlib
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
