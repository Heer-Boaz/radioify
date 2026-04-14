[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [switch]$Rebuild,
    [switch]$SkipBuild,
    [switch]$ReplaceExisting,
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
        [switch]$Rebuild,
        [switch]$SkipBuild,
        [switch]$ReplaceExisting,
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
    if ($Rebuild) {
        $argumentList += "-Rebuild"
    }
    if ($SkipBuild) {
        $argumentList += "-SkipBuild"
    }
    if ($ReplaceExisting) {
        $argumentList += "-ReplaceExisting"
    }
    if ($SkipExplorerRestart) {
        $argumentList += "-SkipExplorerRestart"
    }
    $argumentList += @("-LogPath", $LogPath)

    try {
        $process = Start-Process -FilePath $hostPath -ArgumentList $argumentList -Verb RunAs -Wait -PassThru
    } catch {
        throw "Elevation was cancelled or failed. Re-run .\\install_win11_explorer_integration.ps1 and accept the UAC prompt."
    }

    $exitCode = [int]$process.ExitCode
    if ($exitCode -ne 0) {
        throw "Elevated install failed with exit code $exitCode."
    }
}

$repoRoot = $PSScriptRoot
$buildScript = Join-Path $repoRoot "build.ps1"
$installScript = Join-Path $repoRoot "scripts\windows\install_radioify_win11_context_menu.ps1"
$transcriptStarted = $false

if (-not $LogPath) {
    $LogPath = Resolve-Win11ExplorerIntegrationLogPath -LeafName "win11-explorer-install.log"
}

if (-not (Test-Path -LiteralPath $buildScript)) {
    throw "Build script not found at '$buildScript'."
}
if (-not (Test-Path -LiteralPath $installScript)) {
    throw "Win11 Explorer integration install script not found at '$installScript'."
}

try {
    if (-not $WhatIfPreference -and -not (Test-Win11ExplorerIntegrationAdministrator)) {
        Invoke-Win11ExplorerIntegrationElevated `
            -ScriptPath $PSCommandPath `
            -Rebuild:$Rebuild `
            -SkipBuild:$SkipBuild `
            -ReplaceExisting:$ReplaceExisting `
            -SkipExplorerRestart:$SkipExplorerRestart `
            -LogPath $LogPath
        return
    }

    if (-not $WhatIfPreference) {
        Start-Transcript -Path $LogPath -Force | Out-Null
        $transcriptStarted = $true
    }

    if ($SkipBuild) {
        Write-Host "Skipping build. Reusing existing Win11 Explorer integration artifacts."
    } elseif ($PSCmdlet.ShouldProcess($repoRoot, "Build Radioify and Win11 Explorer integration artifacts")) {
        $buildParams = @{
            Static = $true
            Win11ExplorerIntegration = $true
        }
        if ($Rebuild) {
            $buildParams.Rebuild = $true
        }

        & $buildScript @buildParams
        if ($LASTEXITCODE -ne 0) {
            throw "Build failed with exit code $LASTEXITCODE."
        }
    }

    if ($WhatIfPreference) {
        [void]$PSCmdlet.ShouldProcess($repoRoot, "Install Radioify Windows 11 Explorer integration")
        return
    }

    $installParams = @{}
    if ($ReplaceExisting) {
        $installParams.ReplaceExisting = $true
    }
    if ($SkipExplorerRestart) {
        $installParams.SkipExplorerRestart = $true
    }

    & $installScript @installParams
    if ($LASTEXITCODE -ne 0) {
        throw "Windows 11 Explorer integration install failed with exit code $LASTEXITCODE."
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
