[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [switch]$Rebuild,
    [switch]$SkipBuild,
    [switch]$ReplaceExisting,
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
    $argumentList += @("-LogPath", $LogPath, "-PauseOnError")

    try {
        $process = Start-Process -FilePath $hostPath -ArgumentList $argumentList -Verb RunAs -Wait -PassThru
    } catch {
        throw "Elevation was cancelled or failed. Re-run .\\install_win11_explorer_integration.ps1 and accept the UAC prompt."
    }

    exit ([int]$process.ExitCode)
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

if (-not $WhatIfPreference -and -not (Test-Win11ExplorerIntegrationAdministrator)) {
    Invoke-Win11ExplorerIntegrationElevated `
        -ScriptPath $PSCommandPath `
        -Rebuild:$Rebuild `
        -SkipBuild:$SkipBuild `
        -ReplaceExisting:$ReplaceExisting `
        -SkipExplorerRestart:$SkipExplorerRestart `
        -LogPath $LogPath
}

try {
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
    Write-Host "Log saved to: $LogPath"
    if ($PauseOnError -and -not $WhatIfPreference) {
        [void](Read-Host "Install failed. Press Enter to close this window")
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
