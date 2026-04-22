[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$PackageRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "RadioifyWindowsShellCommon.ps1")
. (Join-Path $PSScriptRoot "RadioifyWindowsBundleMsixInstall.ps1")
. (Join-Path $PSScriptRoot "RadioifyWindowsExplorerIntegrationHost.ps1")

Assert-RadioifyWindowsHost

$resolvedPackageRoot = Resolve-RadioifyWindowsBundleRoot `
    -PackageRoot $PackageRoot `
    -ScriptRoot $PSScriptRoot
$integrationDir = Resolve-RadioifyWindowsBundleIntegrationDirectory -PackageRoot $resolvedPackageRoot
$installScript = Join-Path $PSScriptRoot "install_radioify_win11_context_menu.ps1"
$didInstall = $false

if ($PSCmdlet.ShouldProcess($resolvedPackageRoot, "Install Radioify full MSIX package")) {
    Invoke-RadioifyWindowsExplorerIntegrationScript `
        -ScriptPath $installScript `
        -Operation install `
        -Parameters @{
            IntegrationDir = $integrationDir
            ReplaceExisting = $true
        } `
        -UserCommandHint ".\install_radioify.ps1"

    $didInstall = $true
}

if ($didInstall) {
    Write-Host "Installed Radioify:"
    Write-Host " - Package root: $resolvedPackageRoot"
    Write-Host " - Windows app package: installed"
} elseif ($WhatIfPreference) {
    Write-Host "What if: Radioify install was not performed."
}
