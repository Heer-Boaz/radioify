[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$InstallDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

& (Join-Path $PSScriptRoot "scripts\windows\install_radioify_windows_bundle.ps1") `
    -PackageRoot $PSScriptRoot `
    -InstallDir $InstallDir `
    -WhatIf:$WhatIfPreference
