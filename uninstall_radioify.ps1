[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$PackageDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$uninstallScript = Join-Path $PSScriptRoot "scripts\windows\uninstall_radioify_windows_bundle.ps1"
if (-not $PackageDir) {
    $PackageDir = Join-Path $PSScriptRoot "dist\packages\Radioify-Windows-x64"
}

& $uninstallScript `
    -PackageRoot $PackageDir `
    -WhatIf:$WhatIfPreference
