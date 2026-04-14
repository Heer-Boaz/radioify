[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$InstallDir,
    [string]$PackageDir,
    [switch]$SkipPackage,
    [switch]$Rebuild,
    [switch]$InstallDeps,
    [string]$Version
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = $PSScriptRoot
$packageScript = Join-Path $repoRoot "package_windows.ps1"
$installScript = Join-Path $repoRoot "scripts\windows\install_radioify_windows_bundle.ps1"

if (-not $PackageDir) {
    $PackageDir = Join-Path $repoRoot "dist\packages\Radioify-Windows-x64"
}

$manifestPath = Join-Path $PackageDir "RadioifyPackage.json"
if (-not $SkipPackage -and -not (Test-Path -LiteralPath $manifestPath)) {
    $packageArgs = @()
    if ($Rebuild) {
        $packageArgs += "-Rebuild"
    }
    if ($InstallDeps) {
        $packageArgs += "-InstallDeps"
    }
    if ($Version) {
        $packageArgs += @("-Version", $Version)
    }

    & $packageScript @packageArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Packaging failed with exit code $LASTEXITCODE."
    }
}

if (-not (Test-Path -LiteralPath $manifestPath)) {
    throw "Radioify package bundle not found at '$PackageDir'. Run .\package_windows.ps1 first or omit -SkipPackage."
}

& $installScript -PackageRoot $PackageDir -InstallDir $InstallDir -WhatIf:$WhatIfPreference
