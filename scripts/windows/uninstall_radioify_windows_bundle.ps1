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
$uninstallScript = Join-Path $PSScriptRoot "uninstall_radioify_win11_context_menu.ps1"
$didUninstall = $false

if ($PSCmdlet.ShouldProcess($resolvedPackageRoot, "Uninstall Radioify full MSIX package")) {
    Invoke-RadioifyWindowsExplorerIntegrationScript `
        -ScriptPath $uninstallScript `
        -Operation uninstall `
        -Parameters @{
            IntegrationDir = $integrationDir
        } `
        -UserCommandHint ".\uninstall_radioify.ps1"
    $didUninstall = $true
}

if ($didUninstall) {
    Write-Host "Uninstalled Radioify."
} elseif ($WhatIfPreference) {
    Write-Host "What if: Radioify uninstall was not performed."
}
