[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [switch]$SkipExplorerRestart,
    [string]$LogPath
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

function Get-Win11ExplorerIntegrationLogExcerpt {
    param(
        [Parameter(Mandatory = $true)][string]$LogPath,
        [int]$TailLines = 60
    )

    if (-not (Test-Path -LiteralPath $LogPath)) {
        return $null
    }

    $logFile = Get-Item -LiteralPath $LogPath -ErrorAction SilentlyContinue
    if (-not $logFile) {
        return $null
    }

    if ($logFile.LastWriteTime -lt (Get-Date).AddMinutes(-10)) {
        return $null
    }

    $lines = Get-Content -LiteralPath $LogPath -Tail $TailLines -ErrorAction SilentlyContinue |
        Where-Object {
            -not [string]::IsNullOrWhiteSpace($_) -and
            $_ -notmatch '^\*{6,}$'
        }

    if (-not $lines) {
        return $null
    }

    return ($lines -join [Environment]::NewLine)
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
    $argumentList += @("-LogPath", $LogPath)

    try {
        $process = Start-Process -FilePath $hostPath -ArgumentList $argumentList -Verb RunAs -Wait -PassThru
    } catch {
        throw "Elevation was cancelled or failed. Re-run .\\uninstall_win11_explorer_integration.ps1 and accept the UAC prompt."
    }

    $exitCode = [int]$process.ExitCode
    if ($exitCode -ne 0) {
        throw "Elevated uninstall failed with exit code $exitCode."
    }
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

try {
    if (-not $WhatIfPreference -and -not (Test-Win11ExplorerIntegrationAdministrator)) {
        Invoke-Win11ExplorerIntegrationElevated `
            -ScriptPath $PSCommandPath `
            -SkipExplorerRestart:$SkipExplorerRestart `
            -LogPath $LogPath
        return
    }

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
    $excerpt = Get-Win11ExplorerIntegrationLogExcerpt -LogPath $LogPath
    if ($excerpt) {
        Write-Host "Recent log output:"
        Write-Host $excerpt
    }
    Write-Host "Log saved to: $LogPath"
    exit 1
} finally {
    if ($transcriptStarted) {
        try {
            Stop-Transcript | Out-Null
        } catch {
        }
    }
}
