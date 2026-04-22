[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$IntegrationDir,
    [switch]$ReplaceExisting,
    [string]$LogPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "RadioifyWindowsExplorerIntegrationHost.ps1")
. (Join-Path $PSScriptRoot "RadioifyWin11PackageCommon.ps1")

Assert-RadioifyWindowsHost
if (-not $WhatIfPreference -and -not (Test-RadioifyAdministrator)) {
    throw "Run this install script from an elevated PowerShell window. The MSIX signing certificate must be trusted in Cert:\LocalMachine\TrustedPeople."
}

if (-not $LogPath) {
    $LogPath = Resolve-RadioifyWindowsExplorerIntegrationLogPath -LeafName "win11-explorer-install.log"
}

$resolvedIntegrationDir = Resolve-RadioifyWin11IntegrationDirectory `
    -IntegrationDir $IntegrationDir `
    -ScriptRoot $PSScriptRoot
Assert-RadioifyWin11ArtifactsExist -IntegrationDir $resolvedIntegrationDir
$manifestInfo = Get-RadioifyWin11ManifestInfo -IntegrationDir $resolvedIntegrationDir
$packagePath = Get-RadioifyWin11PackagePath `
    -IntegrationDir $resolvedIntegrationDir `
    -PackageName $manifestInfo.PackageName
$didChangePackage = $false
$transcriptStarted = $false

function Require-RadioifyWin11InstallPackage {
    if (-not (Test-Path -LiteralPath $packagePath)) {
        throw "Signed MSIX package not found at '$packagePath'. Build the Windows package so the .msix and .cer are produced before install."
    }

    Write-Host "Using packaged MSIX: $packagePath"
    Ensure-RadioifyWin11PackagedMsixTrusted -IntegrationDir $resolvedIntegrationDir | Out-Null
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

    $installedPackage = Get-InstalledRadioifyWin11Package -PackageName $manifestInfo.PackageName
    if ($installedPackage -and -not $ReplaceExisting) {
        Write-Host "Radioify MSIX package is already installed:"
        Write-Host "  $($installedPackage.PackageFullName)"
        Write-Host "No package changes were made. Use -ReplaceExisting to force an update."
        return
    }

    if ($PSCmdlet.ShouldProcess($packagePath, "Trust packaged Radioify MSIX certificate")) {
        Require-RadioifyWin11InstallPackage
    }

    if ($PSCmdlet.ShouldProcess($packagePath, "Install Radioify MSIX package")) {
        Install-RadioifyWin11Package -PackagePath $packagePath
        Wait-RadioifyWin11PackageState `
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
