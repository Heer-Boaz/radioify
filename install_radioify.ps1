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
$installScript = Join-Path $repoRoot "scripts\windows\install_radioify_windows_bundle.ps1"
$explicitPackageDir = $PSBoundParameters.ContainsKey("PackageDir")
$hasPackagingOverrides = $Rebuild -or $InstallDeps -or (-not [string]::IsNullOrWhiteSpace($Version))

if ($SkipPackage -and $hasPackagingOverrides) {
    throw "Cannot combine -SkipPackage with -Rebuild, -InstallDeps, or -Version."
}

if ($explicitPackageDir -and $hasPackagingOverrides) {
    throw "Cannot combine -PackageDir with -Rebuild, -InstallDeps, or -Version. Omit -PackageDir to package the current repo state first."
}

if (-not $SkipPackage -and -not $explicitPackageDir) {
    . (Join-Path $repoRoot "scripts\windows\RadioifyWindowsExplorerIntegrationHost.ps1")

    # A local install needs only the MSIX produced by the build. The portable
    # distribution (including its second copy of the models and ZIP) is an
    # explicit package_windows.ps1 operation.
    $buildParams = @{ Static = $true; Win11ExplorerIntegration = $true }
    if ($Rebuild) {
        $buildParams.Rebuild = $true
    }
    if ($InstallDeps) {
        $buildParams.InstallDeps = $true
    }
    if ($Version) {
        # This has always been a display label, separate from MSIX identity.
        Write-Host "Build label: $Version"
    }
    if ($PSCmdlet.ShouldProcess($repoRoot, "Build Radioify and its local MSIX package")) {
        & (Join-Path $repoRoot "build.ps1") @buildParams
        if ($LASTEXITCODE -ne 0) {
            throw "Build failed with exit code $LASTEXITCODE."
        }
    }
    $integrationDir = Join-Path $repoRoot "dist\win11-explorer-integration"
    if ($PSCmdlet.ShouldProcess($integrationDir, "Install the built Radioify MSIX package")) {
        Write-Host "Installing the built MSIX directly..."
        Invoke-RadioifyWindowsExplorerIntegrationScript `
            -ScriptPath (Join-Path $repoRoot "scripts\windows\install_radioify_msix_package.ps1") `
            -Operation install `
            -Parameters @{ IntegrationDir = $integrationDir; ReplaceExisting = $true } `
            -UserCommandHint ".\install_radioify.ps1" | Out-Host
    }
    return
}

# Keep the explicit prebuilt-bundle entry points compatible.
if (-not $PackageDir) {
    $PackageDir = Join-Path $repoRoot "dist\packages\Radioify-Windows-x64"
}
$manifestPath = Join-Path $PackageDir "RadioifyPackage.json"
if (-not (Test-Path -LiteralPath $manifestPath)) {
    throw "Radioify package bundle not found at '$PackageDir'. Run .\package_windows.ps1 first or omit -SkipPackage."
}

& $installScript `
    -PackageRoot $PackageDir `
    -WhatIf:$WhatIfPreference
