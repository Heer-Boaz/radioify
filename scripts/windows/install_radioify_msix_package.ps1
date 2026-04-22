[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$IntegrationDir,
    [switch]$ReplaceExisting,
    [string]$LogPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "RadioifyWindowsExplorerIntegrationHost.ps1")
. (Join-Path $PSScriptRoot "RadioifyWindowsMsixInstall.ps1")

Assert-RadioifyWindowsHost
if (-not $WhatIfPreference -and -not (Test-RadioifyAdministrator)) {
    throw "Run this install script from an elevated PowerShell window. The MSIX signing certificate must be trusted in Cert:\LocalMachine\TrustedPeople."
}

if (-not $LogPath) {
    $LogPath = Resolve-RadioifyWindowsExplorerIntegrationLogPath -LeafName "win11-explorer-install.log"
}

$resolvedIntegrationDir = Resolve-RadioifyMsixIntegrationDirectory `
    -IntegrationDir $IntegrationDir `
    -ScriptRoot $PSScriptRoot
Assert-RadioifyMsixArtifactsExist -IntegrationDir $resolvedIntegrationDir
$manifestInfo = Get-RadioifyMsixManifestInfo -IntegrationDir $resolvedIntegrationDir
$packagePath = Get-RadioifyMsixPackagePath `
    -IntegrationDir $resolvedIntegrationDir `
    -PackageName $manifestInfo.PackageName
$didChangePackage = $false
$transcriptStarted = $false

function Require-RadioifyMsixInstallPackage {
    if (-not (Test-Path -LiteralPath $packagePath)) {
        throw "Signed MSIX package not found at '$packagePath'. Build the Windows package so the .msix and .cer are produced before install."
    }

    Write-Host "Using packaged MSIX: $packagePath"
    Write-Host "Checking packaged MSIX certificate trust..."
    Ensure-RadioifyPackagedMsixTrusted -IntegrationDir $resolvedIntegrationDir | Out-Null
    Write-Host "MSIX certificate trust is ready."
}

try {
    if (-not $WhatIfPreference) {
        Start-Transcript -Path $LogPath -Force | Out-Null
        $transcriptStarted = $true
    }

    if ($WhatIfPreference) {
        [void]$PSCmdlet.ShouldProcess($manifestInfo.PackageName, "Install Radioify MSIX package")
        return
    }

    $installedPackage = Get-InstalledRadioifyMsixPackage -PackageName $manifestInfo.PackageName
    if ($installedPackage -and -not $ReplaceExisting) {
        Write-Host "Radioify MSIX package is already installed:"
        Write-Host "  $($installedPackage.PackageFullName)"
        Write-Host "No package changes were made. Use -ReplaceExisting to force an update."
        return
    }

    if ($PSCmdlet.ShouldProcess($packagePath, "Trust packaged Radioify MSIX certificate")) {
        Require-RadioifyMsixInstallPackage
    }

    if ($PSCmdlet.ShouldProcess($packagePath, "Install Radioify MSIX package")) {
        Install-RadioifyMsixPackage -PackagePath $packagePath
        Wait-RadioifyMsixPackageState `
            -PackageName $manifestInfo.PackageName `
            -DesiredState Ready `
            -TimeoutSeconds 60
        $didChangePackage = $true
    }

    if ($didChangePackage) {
        Write-Host "Radioify MSIX package installed."
    }
    Write-Host "Package: $packagePath"
} finally {
    if ($transcriptStarted) {
        try {
            Stop-Transcript | Out-Null
        } catch {
        }
    }
}
