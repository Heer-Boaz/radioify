[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$IntegrationDir,
    [switch]$ReplaceExisting,
    [switch]$SkipExplorerRestart
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "RadioifyWin11PackageCommon.ps1")

Assert-RadioifyWindowsHost
if (-not $WhatIfPreference -and -not (Test-RadioifyAdministrator)) {
    throw "Run this install script from an elevated PowerShell window. The MSIX signing certificate must be trusted in Cert:\\LocalMachine\\TrustedPeople."
}

$resolvedIntegrationDir = Resolve-RadioifyWin11IntegrationDirectory `
    -IntegrationDir $IntegrationDir `
    -ScriptRoot $PSScriptRoot
Assert-RadioifyWin11ArtifactsExist -IntegrationDir $resolvedIntegrationDir
$manifestInfo = Get-RadioifyWin11ManifestInfo -IntegrationDir $resolvedIntegrationDir

$makeAppxExe = Resolve-WindowsSdkExecutable -ToolName "makeappx.exe"
$signToolExe = Resolve-WindowsSdkExecutable -ToolName "signtool.exe"

$deployRoot = Get-RadioifyWin11DeployRootPath -IntegrationDir $resolvedIntegrationDir
$packageLayout = Get-RadioifyWin11PackageLayoutPath -IntegrationDir $resolvedIntegrationDir
$packagePath = Get-RadioifyWin11PackagePath -IntegrationDir $resolvedIntegrationDir -PackageName $manifestInfo.PackageName
$certInfo = $null
$shellStopped = $false
$didChangePackage = $false
$alreadyInstalled = $false
$shellRecovered = $false

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

if ($PSCmdlet.ShouldProcess($deployRoot, "Stage Radioify external package contents")) {
    $deployRoot = Initialize-RadioifyWin11DeployRoot -IntegrationDir $resolvedIntegrationDir
}

if ($PSCmdlet.ShouldProcess($packageLayout, "Stage Win11 Explorer integration package layout")) {
    $packageLayout = Initialize-RadioifyWin11PackageLayout -IntegrationDir $resolvedIntegrationDir
}

if ($PSCmdlet.ShouldProcess($manifestInfo.Publisher, "Ensure Win11 Explorer integration signing certificate")) {
    $certInfo = Ensure-RadioifyWin11Certificate -Subject $manifestInfo.Publisher -IntegrationDir $resolvedIntegrationDir
}

if ($PSCmdlet.ShouldProcess($packagePath, "Pack Win11 Explorer integration MSIX")) {
    if (Test-Path -LiteralPath $packagePath) {
        Remove-Item -LiteralPath $packagePath -Force
    }
    Invoke-NativeTool -FilePath $makeAppxExe -Arguments @(
        "pack",
        "/d", $packageLayout,
        "/p", $packagePath,
        "/o",
        "/nv"
    )
}

if ($PSCmdlet.ShouldProcess($packagePath, "Sign Win11 Explorer integration MSIX")) {
    if (-not $certInfo) {
        throw "Signing certificate was not prepared before signing."
    }
    Invoke-NativeTool -FilePath $signToolExe -Arguments @(
        "sign",
        "/fd", "SHA256",
        "/sha1", $certInfo.Certificate.Thumbprint,
        "/s", "My",
        $packagePath
    )
}

if ($WhatIfPreference) {
    [void]$PSCmdlet.ShouldProcess($manifestInfo.PackageName, "Install Win11 Explorer integration package")
    return
}

try {
    $installedPackage = Get-InstalledRadioifyWin11Package -PackageName $manifestInfo.PackageName
    if ($installedPackage -and (Test-RadioifyWin11PackageBusy -Package $installedPackage)) {
        if ($ReplaceExisting -and -not $SkipExplorerRestart) {
            Enter-RadioifyShellMaintenance -Message "Stopping Explorer and packaged COM host before waiting for package servicing to settle..."
        }

        Write-Host "Existing Win11 Explorer integration package is still servicing. Waiting for it to settle..."
        $waitParams = @{
            PackageName    = $manifestInfo.PackageName
            TimeoutSeconds = 60
        }
        if ($shellStopped) {
            $waitParams.KeepShellStopped = $true
            $waitParams.SurrogateAppId = $manifestInfo.SurrogateAppId
        }
        $installedPackage = Wait-RadioifyWin11PackageSettled @waitParams
    }

    if ($installedPackage -and -not $ReplaceExisting) {
        $alreadyInstalled = $true
        Write-Host "Win11 Explorer integration is already installed:"
        Write-Host "  $($installedPackage.PackageFullName)"
        Write-Host "No package changes were made. Use -ReplaceExisting to force a reinstall."
    }

    if ($installedPackage -and $ReplaceExisting -and $PSCmdlet.ShouldProcess($installedPackage.PackageFullName, "Remove existing Win11 Explorer integration package")) {
        if ($SkipExplorerRestart) {
            throw "Cannot use -SkipExplorerRestart while replacing an existing Win11 Explorer integration package. Explorer must be restarted so the packaged shell extension can be unloaded."
        }
        Enter-RadioifyShellMaintenance -Message "Stopping Explorer and packaged COM host before package replacement..."
        Write-Host "Removing existing Win11 Explorer integration package..."
        Remove-RadioifyWin11Package -PackageFullName $installedPackage.PackageFullName
        Wait-RadioifyWin11PackageState `
            -PackageName $manifestInfo.PackageName `
            -DesiredState Absent `
            -TimeoutSeconds 60 `
            -KeepShellStopped `
            -SurrogateAppId $manifestInfo.SurrogateAppId
        $installedPackage = $null
    }

    if (-not $alreadyInstalled -and $PSCmdlet.ShouldProcess($manifestInfo.PackageName, "Install Win11 Explorer integration package")) {
        if (-not (Test-Path -LiteralPath $packagePath)) {
            throw "Package file was not created: '$packagePath'"
        }

        if ($shellStopped) {
            Write-Host "Installing Win11 Explorer integration package while Explorer remains stopped..."
        }
        Write-Host "Installing Win11 Explorer integration package..."
        Install-RadioifyWin11Package `
            -PackagePath $packagePath `
            -ExternalLocation $deployRoot
        Wait-RadioifyWin11PackageState `
            -PackageName $manifestInfo.PackageName `
            -DesiredState Ready `
            -TimeoutSeconds 60
        $didChangePackage = $true
    }

    if (-not $WhatIfPreference) {
        if ($didChangePackage) {
            Invoke-RadioifyShellAssociationRefresh
            if (-not $SkipExplorerRestart) {
                if ($shellStopped) {
                    Write-Host "Starting Explorer after package install completed..."
                    Start-RadioifyExplorerShell
                } else {
                    Write-Host "Restarting Explorer after package install completed..."
                    Restart-RadioifyExplorerShell
                }
                $shellRecovered = $true
            }

            Write-Host "Radioify Windows 11 Explorer integration installed."
        } elseif ($alreadyInstalled) {
            Write-Host "Radioify Windows 11 Explorer integration is already present. No package changes were made."
        }
        Write-Host "Package: $packagePath"
        Write-Host "External location: $deployRoot"
        if ($SkipExplorerRestart) {
            Write-Host "Explorer was not restarted. Restart Explorer manually if the new command does not appear immediately."
        }
    }
} finally {
    if ($shellStopped -and -not $shellRecovered -and -not $SkipExplorerRestart) {
        Write-Warning "Explorer was left stopped by the install path. Starting it again."
        Start-RadioifyExplorerShell
    }
}
