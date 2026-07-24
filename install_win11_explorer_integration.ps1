[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [switch]$Rebuild,
    [switch]$SkipBuild,
    [switch]$ReplaceExisting,
    [switch]$RestartExplorer,
    [string]$LogPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "scripts\windows\RadioifyWindowsExplorerIntegrationHost.ps1")
. (Join-Path $PSScriptRoot "scripts\windows\RadioifyWindowsMsixBuild.ps1")

$repoRoot = $PSScriptRoot
$buildScript = Join-Path $repoRoot "build.ps1"
$installScript = Join-Path $repoRoot "scripts\windows\install_radioify_msix_package.ps1"
$integrationDir = Join-Path $repoRoot "dist\win11-explorer-integration"

if (-not (Test-Path -LiteralPath $buildScript)) {
    throw "Build script not found at '$buildScript'."
}
if (-not (Test-Path -LiteralPath $installScript)) {
    throw "Win11 Explorer integration install script not found at '$installScript'."
}

if ($SkipBuild) {
    Write-Host "Skipping build. Reusing existing Win11 Explorer integration artifacts."
} elseif ($PSCmdlet.ShouldProcess($repoRoot, "Build Radioify and Win11 Explorer integration artifacts")) {
    $buildParams = @{
        Static = $true
        Win11ExplorerIntegration = $true
    }
    if ($Rebuild) {
        $buildParams.Rebuild = $true
    }

    & $buildScript @buildParams
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE."
    }
}

if ($WhatIfPreference) {
    [void]$PSCmdlet.ShouldProcess($integrationDir, "Install Radioify Windows 11 Explorer integration")
    if ($RestartExplorer) {
        [void]$PSCmdlet.ShouldProcess("Windows Explorer", "Restart after installing Radioify shell integration")
    }
    return
}

if ($PSCmdlet.ShouldProcess($integrationDir, "Create signed Radioify Windows shell MSIX")) {
    New-RadioifySignedMsixPackage -IntegrationDir $integrationDir | Out-Null
}

$installParams = @{
    IntegrationDir = $integrationDir
    ReplaceExisting = $true
}
if ($ReplaceExisting) {
    $installParams.ReplaceExisting = $true
}

Invoke-RadioifyWindowsExplorerIntegrationScript `
    -ScriptPath $installScript `
    -Operation install `
    -Parameters $installParams `
    -LogPath $LogPath `
    -UserCommandHint ".\install_win11_explorer_integration.ps1"

if ($RestartExplorer -and
    $PSCmdlet.ShouldProcess("Windows Explorer", "Restart after installing Radioify shell integration")) {
    Write-Host "Restarting Windows Explorer so it reloads the updated shell extension..."
    $restartedExplorer = Restart-RadioifyExplorerShell
    Write-Host "Windows Explorer restarted as PID $($restartedExplorer.ProcessId)."
}
