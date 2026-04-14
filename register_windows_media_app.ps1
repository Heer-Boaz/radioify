[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$ExecutablePath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "scripts\windows\RadioifyWindowsMediaAppRegistration.ps1")

$resolvedExecutable = Resolve-RadioifyWindowsMediaExecutablePath -ExecutablePath $ExecutablePath -ScriptRoot $PSScriptRoot
$state = Register-RadioifyWindowsMediaApp -ExecutablePath $resolvedExecutable -WhatIf:$WhatIfPreference

if ($WhatIfPreference) {
    return
}

Write-Host "Registered Radioify as a Windows media app:"
Write-Host " - Executable: $($state.ExecutablePath)"
Write-Host " - AppUserModelID: $($state.AppUserModelId)"
Write-Host " - Start Menu shortcut: $($state.ShortcutPath)"
