[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$InstallDir,
    [switch]$KeepFiles
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

& (Join-Path $PSScriptRoot "scripts\windows\uninstall_radioify_windows_bundle.ps1") `
    -InstallDir $InstallDir `
    -KeepFiles:$KeepFiles `
    -WhatIf:$WhatIfPreference
