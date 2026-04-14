[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$PackageRoot,
    [string]$InstallDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "RadioifyWindowsDesktopInstall.ps1")
. (Join-Path $PSScriptRoot "RadioifyWindowsExplorerIntegrationHost.ps1")

Assert-RadioifyWindowsHost

$definition = Get-RadioifyWindowsPackageDefinition -PackageRoot $PackageRoot -InstallDir $InstallDir
$didInstall = $false

if ($PSCmdlet.ShouldProcess($definition.InstallDir, "Install Radioify Windows bundle")) {
    Copy-RadioifyWindowsPackageToInstallDirectory -Definition $definition
    Register-RadioifyWindowsMediaApp -ExecutablePath $definition.InstalledExecutablePath | Out-Null
    Register-RadioifyWindowsUninstallEntry -Definition $definition

    if (-not (Test-Path -LiteralPath $definition.InstalledExplorerIntegrationDir)) {
        throw "Packaged Win11 Explorer integration directory not found at '$($definition.InstalledExplorerIntegrationDir)'."
    }
    if (-not (Test-Path -LiteralPath $definition.InstalledExplorerInstallScriptPath)) {
        throw "Packaged Win11 Explorer integration install script not found at '$($definition.InstalledExplorerInstallScriptPath)'."
    }

    Invoke-RadioifyWindowsExplorerIntegrationScript `
        -ScriptPath $definition.InstalledExplorerInstallScriptPath `
        -Operation install `
        -Parameters @{
            IntegrationDir = $definition.InstalledExplorerIntegrationDir
            ReplaceExisting = $true
        } `
        -UserCommandHint ".\install_radioify.ps1"

    $didInstall = $true
}

if ($didInstall) {
    Write-Host "Installed Radioify:"
    Write-Host " - Install dir: $($definition.InstallDir)"
    Write-Host " - Executable: $($definition.InstalledExecutablePath)"
    Write-Host " - Start Menu entry: $script:RadioifyWindowsAppName"
    Write-Host " - Explorer context menu: installed"
} elseif ($WhatIfPreference) {
    Write-Host "What if: Radioify install was not performed."
}
