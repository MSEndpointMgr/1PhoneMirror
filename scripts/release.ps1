[CmdletBinding()]
param(
    [string]$Version,
    [switch]$SkipBuild,
    [switch]$SkipPackage,
    [switch]$SkipSign,
    [switch]$UseLocalCert,
    [string]$SignCertThumbprint,
    [string]$SigningEndpoint = 'https://neu.codesigning.azure.net/',
    [string]$SigningAccount  = 'ASA-1PhoneMirror',
    [string]$SigningProfile  = 'PublicTrust1PhoneMirror',
    [string]$SigningTenantId = '83472170-5be6-45bd-b4a7-464f4d12f820',
    [string]$TimestampUrl = 'http://timestamp.acs.microsoft.com',
    [string]$WingetPackage = 'MSEndpointMgr.1PhoneMirror',
    [string]$ReleaseRepo = 'MSEndpointMgr/1PhoneMirror',
    [switch]$PrintOnly
)

$ErrorActionPreference = 'Stop'

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
