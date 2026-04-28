[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$IntegrationDir,
    [string]$LogPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "RadioifyWindowsExplorerIntegrationHost.ps1")
. (Join-Path $PSScriptRoot "RadioifyWindowsMsixInstall.ps1")

Assert-RadioifyWindowsHost

if (-not $WhatIfPreference -and -not (Test-RadioifyAdministrator)) {
    throw "Run this uninstall script from an elevated PowerShell window."
}

if (-not $LogPath) {
    $LogPath = Resolve-RadioifyWindowsExplorerIntegrationLogPath -LeafName "win11-explorer-uninstall.log"
}

$resolvedIntegrationDir = Resolve-RadioifyMsixIntegrationDirectory `
    -IntegrationDir $IntegrationDir `
    -ScriptRoot $PSScriptRoot
$manifestInfo = Get-RadioifyMsixManifestInfo -IntegrationDir $resolvedIntegrationDir
$didRemovePackage = $false
$transcriptStarted = $false

try {
    if (-not $WhatIfPreference) {
        Start-Transcript -Path $LogPath -Force | Out-Null
        $transcriptStarted = $true
    }

    if ($WhatIfPreference) {
        [void]$PSCmdlet.ShouldProcess($manifestInfo.PackageName, "Remove Radioify MSIX package")
        return
    }

    $installedPackage = Get-InstalledRadioifyMsixPackage -PackageName $manifestInfo.PackageName
    if ($installedPackage) {
        if ($PSCmdlet.ShouldProcess($installedPackage.PackageFullName, "Remove Radioify MSIX package")) {
            Stop-RadioifyExplorerIntegrationSurrogates `
                -ComServerAppId $manifestInfo.ComServerAppId
            Remove-RadioifyMsixPackage `
                -PackageFullName $installedPackage.PackageFullName `
                -PackageName $manifestInfo.PackageName
            Wait-RadioifyMsixPackageState `
                -PackageName $manifestInfo.PackageName `
                -DesiredState Absent `
                -TimeoutSeconds 60
            $didRemovePackage = $true
        }
    } else {
        Write-Host "Radioify MSIX package is not installed."
    }

    if ($didRemovePackage) {
        Write-Host "Radioify MSIX package removed."
    }
} finally {
    if ($transcriptStarted) {
        try {
            Stop-Transcript | Out-Null
        } catch {
        }
    }
}
