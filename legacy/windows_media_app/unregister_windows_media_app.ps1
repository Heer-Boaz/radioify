[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$ExecutablePath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "RadioifyWindowsMediaAppRegistration.ps1")

$resolvedExecutable = Resolve-RadioifyWindowsMediaExecutablePath -ExecutablePath $ExecutablePath -ScriptRoot $PSScriptRoot -AllowMissing
Unregister-RadioifyWindowsMediaApp -ExecutablePath $resolvedExecutable -WhatIf:$WhatIfPreference

if ($WhatIfPreference) {
    return
}

Write-Host "Removed Radioify Windows media app registration."
