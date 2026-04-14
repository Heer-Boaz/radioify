[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$InstallDir,
    [switch]$KeepFiles
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "RadioifyWindowsDesktopInstall.ps1")

Assert-RadioifyWindowsHost

$definition = Get-RadioifyWindowsPackageDefinition -PackageRoot (Join-Path $PSScriptRoot "..\..") -InstallDir $InstallDir

if ($PSCmdlet.ShouldProcess($definition.InstallDir, "Uninstall Radioify Windows bundle")) {
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
}

Write-Host "Uninstalled Radioify."
if ($KeepFiles) {
    Write-Host " - Installed files were left in place."
}
