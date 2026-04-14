[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$IntegrationDir,
    [switch]$ReplaceExisting,
    [switch]$SkipExplorerRestart,
    [string]$LogPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "RadioifyWindowsExplorerIntegrationHost.ps1")
. (Join-Path $PSScriptRoot "RadioifyWin11PackageCommon.ps1")

Assert-RadioifyWindowsHost
if (-not $WhatIfPreference -and -not (Test-RadioifyAdministrator)) {
    throw "Run this install script from an elevated PowerShell window. The MSIX signing certificate must be trusted in Cert:\\LocalMachine\\TrustedPeople."
}

if (-not $LogPath) {
    $LogPath = Resolve-RadioifyWindowsExplorerIntegrationLogPath -LeafName "win11-explorer-install.log"
}

$resolvedIntegrationDir = Resolve-RadioifyWin11IntegrationDirectory `
    -IntegrationDir $IntegrationDir `
    -ScriptRoot $PSScriptRoot
Assert-RadioifyWin11ArtifactsExist -IntegrationDir $resolvedIntegrationDir
$manifestInfo = Get-RadioifyWin11ManifestInfo -IntegrationDir $resolvedIntegrationDir

$makeAppxExe = Resolve-WindowsSdkExecutable -ToolName "makeappx.exe"
$signToolExe = Resolve-WindowsSdkExecutable -ToolName "signtool.exe"

$packageLayout = Get-RadioifyWin11PackageLayoutPath -IntegrationDir $resolvedIntegrationDir
$packagePath = Get-RadioifyWin11PackagePath -IntegrationDir $resolvedIntegrationDir -PackageName $manifestInfo.PackageName
$certInfo = $null
$shellStopped = $false
$didChangePackage = $false
$alreadyInstalled = $false
$shellRecovered = $false
$transcriptStarted = $false

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

function Build-RadioifyWin11Package {
    $script:packageLayout = Initialize-RadioifyWin11PackageLayout -IntegrationDir $resolvedIntegrationDir
    $script:certInfo = Ensure-RadioifyWin11Certificate -Subject $manifestInfo.Publisher -IntegrationDir $resolvedIntegrationDir

    if (Test-Path -LiteralPath $packagePath) {
        Remove-Item -LiteralPath $packagePath -Force
    }
    Invoke-NativeTool -FilePath $makeAppxExe -Arguments @(
        "pack",
        "/d", $script:packageLayout,
        "/p", $packagePath,
        "/o",
        "/nv"
    )

    if (-not $script:certInfo) {
        throw "Signing certificate was not prepared before signing."
    }
    Invoke-NativeTool -FilePath $signToolExe -Arguments @(
        "sign",
        "/fd", "SHA256",
        "/sha1", $script:certInfo.Certificate.Thumbprint,
        "/s", "My",
        $packagePath
    )
}

try {
    if (-not $WhatIfPreference) {
        Start-Transcript -Path $LogPath -Force | Out-Null
        $transcriptStarted = $true
    }

    if ($WhatIfPreference) {
        [void]$PSCmdlet.ShouldProcess($manifestInfo.PackageName, "Install Win11 Explorer integration package")
        return
    }

    $installedPackage = Get-InstalledRadioifyWin11Package -PackageName $manifestInfo.PackageName
    if ($installedPackage -and $ReplaceExisting -and $SkipExplorerRestart) {
        throw "Cannot use -SkipExplorerRestart while replacing an existing Win11 Explorer integration package. Explorer must be restarted so the packaged shell extension can be unloaded."
    }

    if ($installedPackage -and (Test-RadioifyWin11PackageBusy -Package $installedPackage)) {
        if ($ReplaceExisting) {
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

    if (-not $alreadyInstalled -and $PSCmdlet.ShouldProcess($packagePath, "Build Win11 Explorer integration package")) {
        Build-RadioifyWin11Package
    }

    if ($installedPackage -and $ReplaceExisting -and $PSCmdlet.ShouldProcess($installedPackage.PackageFullName, "Remove existing Win11 Explorer integration package")) {
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

        $deployRoot = Initialize-RadioifyWin11DeployRoot -IntegrationDir $resolvedIntegrationDir

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
        Write-Host "External location: $(Get-RadioifyWin11DeployRootPath -IntegrationDir $resolvedIntegrationDir)"
        if ($SkipExplorerRestart) {
            Write-Host "Explorer was not restarted. Restart Explorer manually if the new command does not appear immediately."
        }
    }
} finally {
    if ($transcriptStarted) {
        try {
            Stop-Transcript | Out-Null
        } catch {
        }
    }

    if ($shellStopped -and -not $shellRecovered -and -not $SkipExplorerRestart) {
        Write-Warning "Explorer was left stopped by the install path. Starting it again."
        Start-RadioifyExplorerShell
    }
}
