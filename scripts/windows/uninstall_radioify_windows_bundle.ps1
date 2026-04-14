[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$InstallDir,
    [switch]$KeepFiles
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "RadioifyWindowsDesktopInstall.ps1")
. (Join-Path $PSScriptRoot "RadioifyWindowsExplorerIntegrationHost.ps1")

Assert-RadioifyWindowsHost

$definition = Get-RadioifyWindowsInstalledAppDefinition -InstallDir $InstallDir
$didUninstall = $false

if ($PSCmdlet.ShouldProcess($definition.InstallDir, "Uninstall Radioify Windows bundle")) {
    if (Test-Path -LiteralPath $definition.InstalledExplorerIntegrationDir) {
        if (-not (Test-Path -LiteralPath $definition.InstalledExplorerUninstallScriptPath)) {
            throw "Installed Win11 Explorer integration uninstall script not found at '$($definition.InstalledExplorerUninstallScriptPath)'."
        }

        Invoke-RadioifyWindowsExplorerIntegrationScript `
            -ScriptPath $definition.InstalledExplorerUninstallScriptPath `
            -Operation uninstall `
            -Parameters @{
                IntegrationDir = $definition.InstalledExplorerIntegrationDir
            } `
            -UserCommandHint ".\uninstall_radioify.ps1"
    }

    Unregister-RadioifyWindowsMediaApp -ExecutablePath $definition.InstalledExecutablePath
    Unregister-RadioifyWindowsUninstallEntry -Definition $definition

    if (-not $KeepFiles -and (Test-Path -LiteralPath $definition.InstallDir)) {
        $scriptPath = [System.IO.Path]::GetFullPath($PSCommandPath)
        $installDirWithSlash = $definition.InstallDir.TrimEnd('\') + '\'
        $runningFromInstallTree = $scriptPath.StartsWith($installDirWithSlash, [System.StringComparison]::OrdinalIgnoreCase)

        if ($runningFromInstallTree) {
            Invoke-RadioifyWindowsDeferredDirectoryRemoval -Path $definition.InstallDir
        } else {
            Remove-Item -LiteralPath $definition.InstallDir -Recurse -Force
        }
    }

    $didUninstall = $true
}

if ($didUninstall) {
    Write-Host "Uninstalled Radioify."
    if ($KeepFiles) {
        Write-Host " - Installed files were left in place."
    }
} elseif ($WhatIfPreference) {
    Write-Host "What if: Radioify uninstall was not performed."
}
