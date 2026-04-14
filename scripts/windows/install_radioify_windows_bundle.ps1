[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$PackageRoot,
    [string]$InstallDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "RadioifyWindowsDesktopInstall.ps1")

Assert-RadioifyWindowsHost

$definition = Get-RadioifyWindowsPackageDefinition -PackageRoot $PackageRoot -InstallDir $InstallDir

if ($PSCmdlet.ShouldProcess($definition.InstallDir, "Install Radioify Windows bundle")) {
    Copy-RadioifyWindowsPackageToInstallDirectory -Definition $definition
    Register-RadioifyWindowsMediaApp -ExecutablePath $definition.InstalledExecutablePath | Out-Null
    Register-RadioifyWindowsUninstallEntry -Definition $definition
}

Write-Host "Installed Radioify:"
Write-Host " - Install dir: $($definition.InstallDir)"
Write-Host " - Executable: $($definition.InstalledExecutablePath)"
Write-Host " - Start Menu entry: $script:RadioifyWindowsAppName"
