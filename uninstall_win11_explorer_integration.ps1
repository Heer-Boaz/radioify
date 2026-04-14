[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [switch]$SkipExplorerRestart,
    [string]$LogPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "scripts\windows\RadioifyWindowsExplorerIntegrationHost.ps1")

$repoRoot = $PSScriptRoot
$uninstallScript = Join-Path $repoRoot "scripts\windows\uninstall_radioify_win11_context_menu.ps1"
$integrationDir = Join-Path $repoRoot "dist\win11-explorer-integration"

if (-not (Test-Path -LiteralPath $uninstallScript)) {
    throw "Win11 Explorer integration uninstall script not found at '$uninstallScript'."
}

if ($WhatIfPreference) {
    [void]$PSCmdlet.ShouldProcess($integrationDir, "Uninstall Radioify Windows 11 Explorer integration")
    return
}

$uninstallParams = @{
    IntegrationDir = $integrationDir
}
if ($SkipExplorerRestart) {
    $uninstallParams.SkipExplorerRestart = $true
}

Invoke-RadioifyWindowsExplorerIntegrationScript `
    -ScriptPath $uninstallScript `
    -Operation uninstall `
    -Parameters $uninstallParams `
    -LogPath $LogPath `
    -UserCommandHint ".\uninstall_win11_explorer_integration.ps1"
