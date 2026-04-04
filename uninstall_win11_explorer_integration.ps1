[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [switch]$SkipExplorerRestart,
    [string]$LogPath,
    [switch]$PauseOnError
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-Win11ExplorerIntegrationLogPath {
    param([Parameter(Mandatory = $true)][string]$LeafName)

    $localAppData = [Environment]::GetFolderPath("LocalApplicationData")
    if ([string]::IsNullOrWhiteSpace($localAppData)) {
        return (Join-Path $PSScriptRoot $LeafName)
    }

    $logDir = Join-Path $localAppData "Radioify"
    if (-not (Test-Path -LiteralPath $logDir)) {
        New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    }

    return (Join-Path $logDir $LeafName)
}

function Test-Win11ExplorerIntegrationAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Invoke-Win11ExplorerIntegrationElevated {
    param(
        [Parameter(Mandatory = $true)][string]$ScriptPath,
        [switch]$SkipExplorerRestart,
        [Parameter(Mandatory = $true)][string]$LogPath
    )

    $hostPath = (Get-Process -Id $PID).Path
    if (-not $hostPath) {
        $hostPath = "powershell.exe"
    }

    $argumentList = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", $ScriptPath
    )
    if ($SkipExplorerRestart) {
        $argumentList += "-SkipExplorerRestart"
    }
    $argumentList += @("-LogPath", $LogPath, "-PauseOnError")

    try {
        $process = Start-Process -FilePath $hostPath -ArgumentList $argumentList -Verb RunAs -Wait -PassThru
    } catch {
        throw "Elevation was cancelled or failed. Re-run .\\uninstall_win11_explorer_integration.ps1 and accept the UAC prompt."
    }

    exit ([int]$process.ExitCode)
}

$repoRoot = $PSScriptRoot
$uninstallScript = Join-Path $repoRoot "scripts\windows\uninstall_radioify_win11_context_menu.ps1"
$transcriptStarted = $false

if (-not $LogPath) {
    $LogPath = Resolve-Win11ExplorerIntegrationLogPath -LeafName "win11-explorer-uninstall.log"
}

if (-not (Test-Path -LiteralPath $uninstallScript)) {
    throw "Win11 Explorer integration uninstall script not found at '$uninstallScript'."
}

if (-not $WhatIfPreference -and -not (Test-Win11ExplorerIntegrationAdministrator)) {
    Invoke-Win11ExplorerIntegrationElevated `
        -ScriptPath $PSCommandPath `
        -SkipExplorerRestart:$SkipExplorerRestart `
        -LogPath $LogPath
}

try {
    if (-not $WhatIfPreference) {
        Start-Transcript -Path $LogPath -Force | Out-Null
        $transcriptStarted = $true
    }

    if ($WhatIfPreference) {
        [void]$PSCmdlet.ShouldProcess($repoRoot, "Uninstall Radioify Windows 11 Explorer integration")
        return
    }

    $uninstallParams = @{}
    if ($SkipExplorerRestart) {
        $uninstallParams.SkipExplorerRestart = $true
    }

    & $uninstallScript @uninstallParams
    if ($LASTEXITCODE -ne 0) {
        throw "Windows 11 Explorer integration uninstall failed with exit code $LASTEXITCODE."
    }
} catch {
    Write-Error $_
    Write-Host "Log saved to: $LogPath"
    if ($PauseOnError -and -not $WhatIfPreference) {
        [void](Read-Host "Uninstall failed. Press Enter to close this window")
    }
    exit 1
} finally {
    if ($transcriptStarted) {
        try {
            Stop-Transcript | Out-Null
        } catch {
        }
    }
}
