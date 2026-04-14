Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-RadioifyWindowsExplorerIntegrationLogPath {
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

function Get-RadioifyWindowsExplorerIntegrationLogExcerpt {
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

function Test-RadioifyWindowsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Add-RadioifyWindowsExplorerIntegrationArgumentList {
    param(
        [Parameter(Mandatory = $true)][System.Collections.IList]$ArgumentList,
        [hashtable]$Parameters
    )

    if (-not $Parameters) {
        return
    }

    foreach ($entry in $Parameters.GetEnumerator()) {
        $name = [string]$entry.Key
        $value = $entry.Value
        if ([string]::IsNullOrWhiteSpace($name) -or $null -eq $value) {
            continue
        }

        if ($value -is [switch]) {
            if ($value.IsPresent) {
                [void]$ArgumentList.Add("-$name")
            }
            continue
        }

        if ($value -is [bool]) {
            if ($value) {
                [void]$ArgumentList.Add("-$name")
            }
            continue
        }

        if ($value -is [string] -and [string]::IsNullOrWhiteSpace($value)) {
            continue
        }

        [void]$ArgumentList.Add("-$name")
        [void]$ArgumentList.Add([string]$value)
    }
}

function Invoke-RadioifyWindowsExplorerIntegrationScript {
    param(
        [Parameter(Mandatory = $true)][string]$ScriptPath,
        [Parameter(Mandatory = $true)][ValidateSet("install", "uninstall")][string]$Operation,
        [hashtable]$Parameters,
        [string]$LogPath,
        [string]$UserCommandHint
    )

    if (-not (Test-Path -LiteralPath $ScriptPath)) {
        throw "Win11 Explorer integration script not found at '$ScriptPath'."
    }

    if (-not $LogPath) {
        $leafName = if ($Operation -eq "install") {
            "win11-explorer-install.log"
        } else {
            "win11-explorer-uninstall.log"
        }
        $LogPath = Resolve-RadioifyWindowsExplorerIntegrationLogPath -LeafName $leafName
    }

    $resolvedScriptPath = (Resolve-Path -LiteralPath $ScriptPath).Path
    $invocationParams = @{}
    if ($Parameters) {
        foreach ($entry in $Parameters.GetEnumerator()) {
            if ($null -ne $entry.Value) {
                $invocationParams[$entry.Key] = $entry.Value
            }
        }
    }
    $invocationParams.LogPath = $LogPath

    if (Test-RadioifyWindowsAdministrator) {
        & $resolvedScriptPath @invocationParams
        return $LogPath
    }

    $hostPath = (Get-Process -Id $PID).Path
    if (-not $hostPath) {
        $hostPath = "powershell.exe"
    }

    $argumentList = [System.Collections.Generic.List[string]]::new()
    [void]$argumentList.Add("-NoProfile")
    [void]$argumentList.Add("-ExecutionPolicy")
    [void]$argumentList.Add("Bypass")
    [void]$argumentList.Add("-File")
    [void]$argumentList.Add($resolvedScriptPath)
    Add-RadioifyWindowsExplorerIntegrationArgumentList -ArgumentList $argumentList -Parameters $Parameters
    [void]$argumentList.Add("-LogPath")
    [void]$argumentList.Add($LogPath)

    if ([string]::IsNullOrWhiteSpace($UserCommandHint)) {
        $UserCommandHint = $resolvedScriptPath
    }

    try {
        $process = Start-Process -FilePath $hostPath -ArgumentList $argumentList.ToArray() -Verb RunAs -Wait -PassThru
    } catch {
        throw "Elevation was cancelled or failed. Re-run $UserCommandHint and accept the UAC prompt."
    }

    $exitCode = [int]$process.ExitCode
    if ($exitCode -eq 0) {
        return $LogPath
    }

    $excerpt = Get-RadioifyWindowsExplorerIntegrationLogExcerpt -LogPath $LogPath
    if ($excerpt) {
        throw "Elevated $Operation failed with exit code $exitCode.`nRecent log output:`n$excerpt`nLog saved to: $LogPath"
    }

    throw "Elevated $Operation failed with exit code $exitCode.`nLog saved to: $LogPath"
}
