[CmdletBinding(SupportsShouldProcess = $true)]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$installScript = Join-Path $PSScriptRoot "radioify_shell_integration.ps1"
if (-not (Test-Path -LiteralPath $installScript)) {
    throw "Shell integration script not found at '$installScript'."
}

& $installScript `
    -Uninstall `
    -WhatIf:$([bool]$WhatIfPreference) `
    -Verbose:($VerbosePreference -eq [System.Management.Automation.ActionPreference]::Continue)
