Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RadioifyWindowsPackageManifestName = "RadioifyPackage.json"
$script:RadioifyWindowsExplorerIntegrationDirectoryName = "win11-explorer-integration"

function Resolve-RadioifyWindowsBundleRoot {
    param(
        [string]$PackageRoot,
        [Parameter(Mandatory = $true)][string]$ScriptRoot
    )

    $candidate = $PackageRoot
    if (-not $candidate) {
        $candidate = Join-Path $ScriptRoot "..\.."
    }

    $resolved = [System.IO.Path]::GetFullPath($candidate)
    $manifestPath = Join-Path $resolved $script:RadioifyWindowsPackageManifestName
    if (-not (Test-Path -LiteralPath $manifestPath)) {
        throw "Radioify package manifest not found at '$manifestPath'. Extract the packaged bundle first and run the install script from that bundle."
    }

    return $resolved
}

function Resolve-RadioifyWindowsBundleIntegrationDirectory {
    param([Parameter(Mandatory = $true)][string]$PackageRoot)

    $integrationDir = Join-Path $PackageRoot $script:RadioifyWindowsExplorerIntegrationDirectoryName
    if (-not (Test-Path -LiteralPath $integrationDir)) {
        throw "Packaged Windows shell integration directory not found at '$integrationDir'."
    }

    return $integrationDir
}
