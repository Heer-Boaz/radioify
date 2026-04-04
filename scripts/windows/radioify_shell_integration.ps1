[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$ExePath,
    [string]$MenuLabel = "Open with Radioify",
    [switch]$IncludeDirectories,
    [switch]$RestartExplorer,
    [switch]$Uninstall
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..\\..")).Path
}

function Get-DefaultExePath {
    $repoRoot = Get-RepoRoot
    return Join-Path $repoRoot "dist\\radioify.exe"
}

function Get-SupportedMediaExtensions {
    $extensions = @(
        ".wav", ".mp3", ".flac", ".m4a", ".webm", ".mp4", ".mov", ".ogg",
        ".kss", ".nsf", ".gsf", ".minigsf", ".mid", ".midi", ".vgm", ".vgz",
        ".psf", ".minipsf", ".psf2", ".minipsf2", ".mkv"
    )

    return $extensions |
        Sort-Object -Unique
}

function Ensure-RegistryKey {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -Path $Path -Force | Out-Null
    }
}

function Set-RegistryDefaultValue {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value
    )

    Ensure-RegistryKey -Path $Path
    Set-Item -LiteralPath $Path -Value $Value
}

function Set-RegistryStringValue {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value
    )

    Ensure-RegistryKey -Path $Path
    New-ItemProperty -LiteralPath $Path -Name $Name -Value $Value `
        -PropertyType String -Force | Out-Null
}

function Remove-RegistryTree {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

function Invoke-ShellAssociationRefresh {
    if (-not ("Radioify.ShellNative" -as [type])) {
        Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

namespace Radioify
{
    public static class ShellNative
    {
        [DllImport("shell32.dll")]
        public static extern void SHChangeNotify(uint wEventId, uint uFlags, IntPtr dwItem1, IntPtr dwItem2);
    }
}
"@
    }

    $SHCNE_ASSOCCHANGED = 0x08000000
    $SHCNF_IDLIST = 0x0000
    [Radioify.ShellNative]::SHChangeNotify($SHCNE_ASSOCCHANGED, $SHCNF_IDLIST, [IntPtr]::Zero, [IntPtr]::Zero)
}

function Restart-ExplorerShell {
    Get-Process explorer -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Process explorer.exe
}

function Install-ApplicationRegistration {
    param(
        [Parameter(Mandatory = $true)][string]$ClassesRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedExePath
    )

    $appKey = Join-Path $ClassesRoot "Applications\\radioify.exe"
    $defaultIconKey = Join-Path $appKey "DefaultIcon"
    $commandKey = Join-Path $appKey "shell\\open\\command"
    $supportedTypesKey = Join-Path $appKey "SupportedTypes"
    $command = "`"$ResolvedExePath`" --shell-open `"%1`""

    if ($PSCmdlet.ShouldProcess($appKey, "Register Radioify application shell entry")) {
        Set-RegistryStringValue -Path $appKey -Name "FriendlyAppName" -Value "Radioify"
        Set-RegistryDefaultValue -Path $defaultIconKey -Value "`"$ResolvedExePath`""
        Set-RegistryDefaultValue -Path $commandKey -Value $command

        foreach ($extension in Get-SupportedMediaExtensions) {
            Set-RegistryStringValue -Path $supportedTypesKey -Name $extension -Value ""
        }
    }
}

function Install-ExtensionContextMenu {
    param(
        [Parameter(Mandatory = $true)][string]$ClassesRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedExePath,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $command = "`"$ResolvedExePath`" --shell-open `"%1`""
    foreach ($extension in Get-SupportedMediaExtensions) {
        $shellKey = Join-Path $ClassesRoot "SystemFileAssociations\\$extension\\shell\\Radioify"
        $commandKey = Join-Path $shellKey "command"

        if ($PSCmdlet.ShouldProcess($shellKey, "Register Radioify context menu for $extension")) {
            Set-RegistryDefaultValue -Path $shellKey -Value $Label
            Set-RegistryStringValue -Path $shellKey -Name "MUIVerb" -Value $Label
            Set-RegistryStringValue -Path $shellKey -Name "Icon" -Value "`"$ResolvedExePath`""
            Set-RegistryDefaultValue -Path $commandKey -Value $command
        }
    }
}

function Install-DirectoryContextMenu {
    param(
        [Parameter(Mandatory = $true)][string]$ClassesRoot,
        [Parameter(Mandatory = $true)][string]$ResolvedExePath,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $shellKey = Join-Path $ClassesRoot "Directory\\shell\\Radioify"
    $commandKey = Join-Path $shellKey "command"
    $command = "`"$ResolvedExePath`" --shell-open `"%1`""

    if ($PSCmdlet.ShouldProcess($shellKey, "Register Radioify context menu for directories")) {
        Set-RegistryDefaultValue -Path $shellKey -Value $Label
        Set-RegistryStringValue -Path $shellKey -Name "MUIVerb" -Value $Label
        Set-RegistryStringValue -Path $shellKey -Name "Icon" -Value "`"$ResolvedExePath`""
        Set-RegistryDefaultValue -Path $commandKey -Value $command
    }
}

function Uninstall-ShellIntegration {
    param([Parameter(Mandatory = $true)][string]$ClassesRoot)

    $paths = @(
        (Join-Path $ClassesRoot "Applications\\radioify.exe"),
        (Join-Path $ClassesRoot "Directory\\shell\\Radioify")
    )

    foreach ($extension in Get-SupportedMediaExtensions) {
        $paths += Join-Path $ClassesRoot "SystemFileAssociations\\$extension\\shell\\Radioify"
    }

    foreach ($path in $paths) {
        if ($PSCmdlet.ShouldProcess($path, "Remove Radioify shell integration")) {
            Remove-RegistryTree -Path $path
        }
    }
}

$classesRoot = "HKCU:\\Software\\Classes"
$targetExePath = if ($ExePath) { $ExePath } else { Get-DefaultExePath }

if (-not $Uninstall) {
    if (-not (Test-Path -LiteralPath $targetExePath)) {
        throw "Radioify executable not found at '$targetExePath'. Build first or pass -ExePath."
    }

    $resolvedExePath = (Resolve-Path -LiteralPath $targetExePath).Path
    Install-ApplicationRegistration -ClassesRoot $classesRoot -ResolvedExePath $resolvedExePath
    Install-ExtensionContextMenu -ClassesRoot $classesRoot -ResolvedExePath $resolvedExePath -Label $MenuLabel
    if ($IncludeDirectories) {
        Install-DirectoryContextMenu -ClassesRoot $classesRoot -ResolvedExePath $resolvedExePath -Label $MenuLabel
    }
    if ($WhatIfPreference) {
        Write-Host "WhatIf: no registry changes were made."
    } else {
        Invoke-ShellAssociationRefresh
        if ($RestartExplorer) {
            Restart-ExplorerShell
        }
        Write-Host "Radioify shell integration installed for supported audio/video extensions under HKCU."
        if ($IncludeDirectories) {
            Write-Host "Directory context menu integration was also installed."
        }
        if (-not $RestartExplorer) {
            Write-Host "If Explorer still does not show the entry immediately, rerun with -RestartExplorer or restart Explorer/sign out once."
        }
    }
} else {
    Uninstall-ShellIntegration -ClassesRoot $classesRoot
    if ($WhatIfPreference) {
        Write-Host "WhatIf: no registry changes were made."
    } else {
        Invoke-ShellAssociationRefresh
        if ($RestartExplorer) {
            Restart-ExplorerShell
        }
        Write-Host "Radioify shell integration removed from HKCU."
    }
}
