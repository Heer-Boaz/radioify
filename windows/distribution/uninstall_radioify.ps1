[CmdletBinding(SupportsShouldProcess = $true)]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

& (Join-Path $PSScriptRoot "scripts\windows\uninstall_radioify_windows_bundle.ps1") `
    -PackageRoot $PSScriptRoot `
    -WhatIf:$WhatIfPreference
