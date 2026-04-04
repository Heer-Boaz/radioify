[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$IntegrationDir,
    [switch]$SkipExplorerRestart
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "RadioifyWin11PackageCommon.ps1")

Assert-RadioifyWindowsHost

$resolvedIntegrationDir = Resolve-RadioifyWin11IntegrationDirectory `
    -IntegrationDir $IntegrationDir `
    -ScriptRoot $PSScriptRoot
$manifestInfo = Get-RadioifyWin11ManifestInfo -IntegrationDir $resolvedIntegrationDir
$shellStopped = $false
$didRemovePackage = $false

function Enter-RadioifyShellMaintenance {
    param([Parameter(Mandatory = $true)][string]$Message)

    if ($script:shellStopped) {
        Stop-RadioifyWin11SurrogateServer -AppId $manifestInfo.SurrogateAppId
        return
    }

    Write-Host $Message
    Stop-RadioifyWin11IntegrationHosts -SurrogateAppId $manifestInfo.SurrogateAppId
    Wait-RadioifyProcessExit -Name "explorer" | Out-Null
    $script:shellStopped = $true
}

if ($WhatIfPreference) {
    [void]$PSCmdlet.ShouldProcess($manifestInfo.PackageName, "Remove Win11 Explorer integration package")
    return
}

$installedPackage = Get-InstalledRadioifyWin11Package -PackageName $manifestInfo.PackageName
if ($installedPackage -and (Test-RadioifyWin11PackageBusy -Package $installedPackage)) {
    if ($SkipExplorerRestart) {
        throw "Cannot use -SkipExplorerRestart while the existing Win11 Explorer integration package is still servicing. Explorer must stay unloaded until servicing settles."
    }

    Enter-RadioifyShellMaintenance -Message "Stopping Explorer and packaged COM host before waiting for package servicing to settle..."
    Write-Host "Existing Win11 Explorer integration package is still servicing. Waiting for it to settle..."
    $installedPackage = Wait-RadioifyWin11PackageSettled `
        -PackageName $manifestInfo.PackageName `
        -TimeoutSeconds 60 `
        -KeepShellStopped `
        -SurrogateAppId $manifestInfo.SurrogateAppId
}

if ($installedPackage) {
    if ($PSCmdlet.ShouldProcess($installedPackage.PackageFullName, "Remove Win11 Explorer integration package")) {
        if ($SkipExplorerRestart) {
            throw "Cannot use -SkipExplorerRestart while uninstalling the Win11 Explorer integration package. Explorer must be restarted so the packaged shell extension can be unloaded."
        }
        Enter-RadioifyShellMaintenance -Message "Stopping Explorer and packaged COM host before package removal..."
        Write-Host "Removing Win11 Explorer integration package..."
        Remove-RadioifyWin11Package -PackageFullName $installedPackage.PackageFullName
        Wait-RadioifyWin11PackageState `
            -PackageName $manifestInfo.PackageName `
            -DesiredState Absent `
            -TimeoutSeconds 60 `
            -KeepShellStopped `
            -SurrogateAppId $manifestInfo.SurrogateAppId
        $didRemovePackage = $true
    }
} else {
    Write-Host "Radioify Windows 11 Explorer integration package is not installed."
}

if (-not $WhatIfPreference -and $didRemovePackage) {
    Invoke-RadioifyShellAssociationRefresh
    if (-not $SkipExplorerRestart) {
        if ($shellStopped) {
            Write-Host "Starting Explorer after package removal completed..."
            Start-RadioifyExplorerShell
        } else {
            Write-Host "Restarting Explorer after package removal completed..."
            Restart-RadioifyExplorerShell
        }
    }
}
