[CmdletBinding()]
<#
.SYNOPSIS
    Produces the signed MSI release artifact and its publish-time SHA-256.

.DESCRIPTION
    Wraps build and packaging so the final published MSI, its SHA-256, and the
    release metadata are generated from the same exact artifact. Azure Trusted
    Signing configuration must be supplied explicitly or via the
    OPM_TRUSTED_SIGNING_* environment variables. Timestamp URLs are validated
    and HTTP is allowed only for approved timestamp providers required by the
    current signing toolchain.
#>
param(
    [string]$Version,
    [switch]$SkipBuild,
    [switch]$SkipPackage,
    [switch]$SkipSign,
    [switch]$UseLocalCert,
    [string]$SignCertThumbprint,
    [string]$SigningEndpoint = $env:OPM_TRUSTED_SIGNING_ENDPOINT,
    [string]$SigningAccount  = $env:OPM_TRUSTED_SIGNING_ACCOUNT,
    [string]$SigningProfile  = $env:OPM_TRUSTED_SIGNING_PROFILE,
    [string]$SigningTenantId = $env:OPM_TRUSTED_SIGNING_TENANT_ID,
    [string]$TimestampUrl,
    [string]$WingetPackage = 'MSEndpointMgr.1PhoneMirror',
    [string]$ReleaseRepo = 'MSEndpointMgr/1PhoneMirror',
    [switch]$PrintOnly
)

$ErrorActionPreference = 'Stop'

function Assert-HttpsUrl {
    param(
        [Parameter(Mandatory)] [string] $Url,
        [Parameter(Mandatory)] [string] $SettingName
    )

    $uri = $null
    if (-not [Uri]::TryCreate($Url, [UriKind]::Absolute, [ref]$uri)) {
        throw "$SettingName must be an absolute URL. Got: $Url"
    }
    if ($uri.Scheme -ne 'https') {
        throw "$SettingName must use HTTPS. Got: $Url"
    }
}

function Assert-TimestampUrl {
    param(
        [Parameter(Mandatory)] [string] $Url
    )

    $uri = $null
    if (-not [Uri]::TryCreate($Url, [UriKind]::Absolute, [ref]$uri)) {
        throw "TimestampUrl must be an absolute URL. Got: $Url"
    }
    if ($uri.Scheme -eq 'https') { return }
    if ($uri.Scheme -ne 'http') {
        throw "TimestampUrl must use HTTP or HTTPS. Got: $Url"
    }

    $approvedHosts = @(
        'timestamp.acs.microsoft.com',
        'timestamp.digicert.com'
    )
    if ($approvedHosts -notcontains $uri.Host) {
        throw "HTTP TimestampUrl is only allowed for approved timestamp providers. Got: $Url"
    }
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildScript = Join-Path $root 'scripts\build.ps1'
$packageScript = Join-Path $root 'package.ps1'
$distDir = Join-Path $root 'dist'

function Get-VersionFromCmake {
    $cmakePath = Join-Path $root 'CMakeLists.txt'
    if (-not (Test-Path $cmakePath)) { return $null }

    $m = Select-String -Path $cmakePath -Pattern 'project\s*\([^\)]*VERSION\s+(\d+\.\d+\.\d+)' |
        Select-Object -First 1
    if ($m) { return $m.Matches[0].Groups[1].Value }
    return $null
}

if (-not $Version) {
    $Version = Get-VersionFromCmake
}
if (-not $Version) {
    throw 'Version is required. Pass -Version or ensure CMakeLists.txt has a project(... VERSION x.y.z ...) line.'
}

$artifactName = "1PhoneMirror-$Version.msi"
$artifactPath = Join-Path $distDir $artifactName
$hashFilePath = "$artifactPath.sha256"
$releaseJsonPath = Join-Path $distDir "1PhoneMirror-$Version.release.json"

if (-not $PSBoundParameters.ContainsKey('TimestampUrl')) {
    $TimestampUrl = if ($UseLocalCert) { 'http://timestamp.digicert.com' } else { 'http://timestamp.acs.microsoft.com' }
}
Assert-TimestampUrl -Url $TimestampUrl

if (-not $SkipBuild -and -not $SkipPackage) {
    Write-Host "==> Building Release binary" -ForegroundColor Cyan
    & $buildScript -Config Release
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
}

if (-not $SkipPackage) {
    Write-Host "==> Packaging MSI" -ForegroundColor Cyan
    if ($SkipSign) {
        & $packageScript -Version $Version -SkipBuild:$SkipBuild
    }
    elseif ($UseLocalCert) {
        & $packageScript -Version $Version -SkipBuild:$SkipBuild -SignCertThumbprint $SignCertThumbprint -TimestampUrl $TimestampUrl
    }
    else {
        & $packageScript -Version $Version -SkipBuild:$SkipBuild `
            -AzureSign `
            -SigningEndpoint $SigningEndpoint `
            -SigningAccount $SigningAccount `
            -SigningProfile $SigningProfile `
            -SigningTenantId $SigningTenantId `
            -TimestampUrl $TimestampUrl
    }
    if ($LASTEXITCODE -ne 0) { throw 'Packaging failed.' }
}

if (-not (Test-Path $artifactPath)) {
    throw "Final MSI was not found at '$artifactPath'."
}

# IMPORTANT: compute the hash from the exact file that is being published. This is
# the source of truth for winget / GitHub release metadata and prevents the hash
# mismatch issue caused by hashing a local or stale build instead of the final,
# signed, packaged artifact.
$sha256 = (Get-FileHash -Path $artifactPath -Algorithm SHA256).Hash
$fileName = Split-Path -Leaf $artifactPath
Set-Content -Path $hashFilePath -Value "$sha256  $fileName" -Encoding ASCII

$releaseInfo = [ordered]@{
    version = $Version
    artifact = $fileName
    sha256 = $sha256
    publishedAtUtc = (Get-Date).ToString('o')
    source = 'local-release-script'
    wingetPackage = $WingetPackage
    releaseRepo = $ReleaseRepo
    timestampUrl = $TimestampUrl
    signed = (-not $SkipSign)
    signingMethod = if ($SkipSign) { 'none' } elseif ($UseLocalCert) { 'local-cert' } else { 'azure-trusted-signing' }
    signingProfile = if ($SkipSign -or $UseLocalCert) { $null } else { "$SigningAccount/$SigningProfile" }
}
$releaseInfo | ConvertTo-Json | Set-Content -Path $releaseJsonPath -Encoding UTF8

Write-Host "" 
Write-Host "==> Release artifact ready" -ForegroundColor Green
Write-Host "    MSI:          $artifactPath"
Write-Host "    SHA256:       $sha256"
Write-Host "    Hash file:    $hashFilePath"
Write-Host "    Release JSON: $releaseJsonPath"
Write-Host "" 
Write-Host "This hash is computed from the exact file on disk that will be published. Do not publish a different file without recalculating the hash." -ForegroundColor Yellow
Write-Host "" 
Write-Host "Winget integrity check example:" -ForegroundColor Cyan
Write-Host "    (Get-FileHash '$artifactPath' -Algorithm SHA256).Hash"
Write-Host "    wingetcreate update $WingetPackage --version $Version --urls 'https://github.com/$ReleaseRepo/releases/download/v$Version/$fileName' --submit"
Write-Host "" 

if ($PrintOnly) {
    Write-Host 'Dry run complete. No upload was attempted.' -ForegroundColor DarkGray
    return
}

Write-Host "If you want to publish, do it only after the file above is the exact file that is uploaded to GitHub Releases." -ForegroundColor DarkGray
