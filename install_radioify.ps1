[CmdletBinding(SupportsShouldProcess = $true)]
param(
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
$explicitPackageDir = $PSBoundParameters.ContainsKey("PackageDir")
$hasPackagingOverrides = $Rebuild -or $InstallDeps -or (-not [string]::IsNullOrWhiteSpace($Version))

if ($SkipPackage -and $hasPackagingOverrides) {
    throw "Cannot combine -SkipPackage with -Rebuild, -InstallDeps, or -Version."
}

if ($explicitPackageDir -and $hasPackagingOverrides) {
    throw "Cannot combine -PackageDir with -Rebuild, -InstallDeps, or -Version. Omit -PackageDir to package the current repo state first."
}

if (-not $PackageDir) {
    $PackageDir = Join-Path $repoRoot "dist\packages\Radioify-Windows-x64"
}

$manifestPath = Join-Path $PackageDir "RadioifyPackage.json"
if (-not $SkipPackage -and -not $explicitPackageDir) {
    $packageParams = @{}
    if ($Rebuild) {
        $packageParams.Rebuild = $true
    }
    if ($InstallDeps) {
        $packageParams.InstallDeps = $true
    }
    if ($Version) {
        $packageParams.Version = $Version
    }

    $previousWhatIfPreference = $WhatIfPreference
    try {
        $WhatIfPreference = $false
        & $packageScript @packageParams
    }
    finally {
        $WhatIfPreference = $previousWhatIfPreference
    }
}

if (-not (Test-Path -LiteralPath $manifestPath)) {
    throw "Radioify package bundle not found at '$PackageDir'. Run .\package_windows.ps1 first or omit -SkipPackage."
}

& $installScript `
    -PackageRoot $PackageDir `
    -WhatIf:$WhatIfPreference
