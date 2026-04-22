Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "RadioifyWindowsShellCommon.ps1")

function Test-RadioifyAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Resolve-RadioifyMsixIntegrationDirectory {
    param(
        [string]$IntegrationDir,
        [string]$ScriptRoot
    )

    if ($IntegrationDir) {
        return (Resolve-Path -LiteralPath $IntegrationDir).Path
    }

    $localManifest = Join-Path $ScriptRoot "Package.appxmanifest"
    if (Test-Path -LiteralPath $localManifest) {
        return (Resolve-Path -LiteralPath $ScriptRoot).Path
    }

    $repoRoot = (Resolve-Path (Join-Path $ScriptRoot "..\..")).Path
    $candidate = Join-Path $repoRoot "dist\win11-explorer-integration"
    if (Test-Path -LiteralPath $candidate) {
        return (Resolve-Path -LiteralPath $candidate).Path
    }

    throw "Unable to locate the Radioify MSIX metadata directory. Build first with .\build.ps1 -Static -Win11ExplorerIntegration or pass -IntegrationDir."
}

function Get-RadioifyMsixManifestInfo {
    param([Parameter(Mandatory = $true)][string]$IntegrationDir)

    $manifestPath = Join-Path $IntegrationDir "Package.appxmanifest"
    if (-not (Test-Path -LiteralPath $manifestPath)) {
        throw "Package manifest not found at '$manifestPath'. Build first with .\build.ps1 -Static -Win11ExplorerIntegration."
    }

    [xml]$manifestXml = Get-Content -LiteralPath $manifestPath
    return [pscustomobject]@{
        ManifestPath = $manifestPath
        PackageName = [string]$manifestXml.Package.Identity.Name
        Publisher = [string]$manifestXml.Package.Identity.Publisher
        Version = [string]$manifestXml.Package.Identity.Version
    }
}

function New-RadioifyMsixPackageIdentityVersion {
    $now = Get-Date
    return "{0}.{1}.{2}.{3}" -f `
        $now.Year,
        (($now.Month * 100) + $now.Day),
        (($now.Hour * 100) + $now.Minute),
        $now.Second
}

function Resolve-RadioifyMsixPackageIdentityVersion {
    param([string]$Version)

    if ([string]::IsNullOrWhiteSpace($Version)) {
        $Version = New-RadioifyMsixPackageIdentityVersion
    }

    $parts = $Version -split '\.'
    if ($parts.Count -ne 4) {
        throw "MSIX package identity version must have four numeric parts: '$Version'."
    }

    foreach ($part in $parts) {
        $value = 0
        if (-not [int]::TryParse($part, [ref]$value) -or $value -lt 0 -or $value -gt 65535) {
            throw "MSIX package identity version part '$part' is invalid in '$Version'. Expected 0..65535."
        }
    }

    return ($parts -join ".")
}

function Set-RadioifyMsixManifestIdentityVersion {
    param(
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [Parameter(Mandatory = $true)][string]$Version
    )

    $resolvedVersion = Resolve-RadioifyMsixPackageIdentityVersion -Version $Version
    [xml]$manifestXml = Get-Content -LiteralPath $ManifestPath
    $manifestXml.Package.Identity.Version = $resolvedVersion

    $settings = New-Object System.Xml.XmlWriterSettings
    $settings.Indent = $true
    $settings.Encoding = New-Object System.Text.UTF8Encoding($false)

    $writer = [System.Xml.XmlWriter]::Create($ManifestPath, $settings)
    try {
        $manifestXml.Save($writer)
    } finally {
        $writer.Close()
    }
}

function Assert-RadioifyMsixArtifactsExist {
    param([Parameter(Mandatory = $true)][string]$IntegrationDir)

    $requiredPaths = @(
        (Join-Path $IntegrationDir "Package.appxmanifest"),
        (Join-Path $IntegrationDir "radioify_explorer.dll"),
        (Join-Path (Split-Path -Parent $IntegrationDir) "radioify.exe")
    )

    foreach ($path in $requiredPaths) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Required Radioify MSIX artifact is missing: '$path'. Build first with .\build.ps1 -Static -Win11ExplorerIntegration."
        }
    }
}

function Get-RadioifyMsixPackageArtifactDirectory {
    param([Parameter(Mandatory = $true)][string]$IntegrationDir)
    return (Split-Path -Parent $IntegrationDir)
}

function Get-RadioifyMsixPackagePath {
    param(
        [Parameter(Mandatory = $true)][string]$IntegrationDir,
        [Parameter(Mandatory = $true)][string]$PackageName
    )

    return (Join-Path (Get-RadioifyMsixPackageArtifactDirectory -IntegrationDir $IntegrationDir) "$PackageName.msix")
}

function Get-RadioifyMsixCertificatePath {
    param(
        [Parameter(Mandatory = $true)][string]$IntegrationDir,
        [Parameter(Mandatory = $true)][string]$PackageName
    )

    return (Join-Path (Get-RadioifyMsixPackageArtifactDirectory -IntegrationDir $IntegrationDir) "$PackageName.cer")
}

function Clear-RadioifyMsixObsoleteArtifacts {
    param(
        [Parameter(Mandatory = $true)][string]$IntegrationDir,
        [Parameter(Mandatory = $true)][string]$PackageName
    )

    foreach ($obsoletePath in @(
            (Join-Path $IntegrationDir "$PackageName.msix"),
            (Join-Path $IntegrationDir "$PackageName.cer")
        )) {
        if (Test-Path -LiteralPath $obsoletePath) {
            Remove-Item -LiteralPath $obsoletePath -Recurse -Force
        }
    }
}
