Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RadioifyWindowsAppName = "Radioify"
$script:RadioifyWindowsExecutableName = "radioify.exe"

function Assert-RadioifyWindowsHost {
    $platform = [System.Environment]::OSVersion.Platform
    if ($platform -ne [System.PlatformID]::Win32NT) {
        throw "This script must be run on Windows."
    }
}

function Invoke-RadioifyShellAssociationRefresh {
    if (-not ("Radioify.ShellNative" -as [type])) {
        Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
namespace Radioify {
    public static class ShellNative {
        [DllImport("shell32.dll")]
        public static extern void SHChangeNotify(uint wEventId, uint uFlags, IntPtr dwItem1, IntPtr dwItem2);
    }
}
"@
    }

    $shcneAssocChanged = 0x08000000
    $shcnfIdList = 0x0000
    $shcnfFlush = 0x1000
    [Radioify.ShellNative]::SHChangeNotify(
        $shcneAssocChanged,
        ($shcnfIdList -bor $shcnfFlush),
        [IntPtr]::Zero,
        [IntPtr]::Zero)
}

function Restart-RadioifyExplorerShell {
    param([int]$TimeoutSeconds = 10)

    $existingProcesses = @(Get-Process -Name "explorer" -ErrorAction SilentlyContinue)
    $existingProcessIds = @($existingProcesses | ForEach-Object Id)

    foreach ($processId in $existingProcessIds) {
        try {
            Stop-Process -Id $processId -Force -ErrorAction Stop
        } catch {
            if (Get-Process -Id $processId -ErrorAction SilentlyContinue) {
                throw
            }
        }
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $remainingProcessIds = @(
            $existingProcessIds |
                Where-Object {
                    Get-Process -Id $_ -ErrorAction SilentlyContinue
                }
        )
        if ($remainingProcessIds.Count -eq 0) {
            break
        }
        Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $deadline)

    if ($remainingProcessIds.Count -gt 0) {
        throw "Timed out stopping Windows Explorer processes: $($remainingProcessIds -join ', ')"
    }

    $explorerPath = Join-Path $env:WINDIR "explorer.exe"
    Start-Process -FilePath $explorerPath

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $mainShell = Get-CimInstance Win32_Process -Filter "Name='explorer.exe'" -ErrorAction SilentlyContinue |
            Where-Object {
                $_.ProcessId -notin $existingProcessIds -and
                $_.CommandLine -notmatch '(?i)/factory,'
            } |
            Select-Object -First 1
        if ($mainShell) {
            return $mainShell
        }
        Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $deadline)

    throw "Windows Explorer did not restart within $TimeoutSeconds seconds."
}
