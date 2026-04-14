param(
    [ValidateSet("Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Release",
    [switch]$Rebuild,
    [switch]$SkipBuild,
    [switch]$InstallDeps,
    [string]$Version,
    [string]$OutputDir
)

$ErrorActionPreference = "Stop"

$buildRoot = Join-Path $PSScriptRoot "scripts\build"
. (Join-Path $buildRoot "Bootstrap.ps1")
foreach ($modulePath in (Resolve-BuildModulePaths -BuildRoot $buildRoot)) {
    . $modulePath
}
. (Join-Path $PSScriptRoot "scripts\windows\RadioifyWindowsReleasePackaging.ps1")

function Resolve-RadioifyPackagingExecutablePath {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [string]$BuildDir,
        [Parameter(Mandatory = $true)][string]$Config
    )

    if ($BuildDir) {
        $fromContext = Find-RadioifyExecutable -Root $RepoRoot -BuildDir $BuildDir -Config $Config
        if ($fromContext) {
            return $fromContext
        }
    }

    foreach ($candidateBuildDir in @(
            (Join-Path $RepoRoot "build-ninja-clangcl-static"),
            (Join-Path $RepoRoot "build-ninja-clangcl"),
            (Join-Path $RepoRoot "build"),
            (Join-Path $RepoRoot "build-ninja")
        )) {
        $candidate = Find-RadioifyExecutable -Root $RepoRoot -BuildDir $candidateBuildDir -Config $Config
        if ($candidate) {
            return $candidate
        }
    }

    throw "Unable to locate a built radioify.exe. Run .\build.ps1 -Static first or rerun this script without -SkipBuild."
}

try {
    Enable-AnsiConsole

    $context = New-BuildContext `
        -RepoRoot $PSScriptRoot `
        -InitialOptions (New-InitialBuildOptions `
            -Config $Config `
            -Clean $false `
            -Rebuild ([bool]$Rebuild) `
            -Static $true `
            -Ninja $false `
            -ClangCl $false `
            -ClangClExplicitlySet $false `
            -VerboseBuild $false `
            -InstallDeps ([bool]$InstallDeps) `
            -VcpkgRoot $null `
            -MelodyAnalysis $false `
            -Win11ExplorerIntegration $false `
            -TimingLog $false `
            -StagingUpload $false `
            -VideoErrorLog $false `
            -FfmpegErrorLog $false) `
        -RemainingArguments @()

    if (-not $SkipBuild) {
        $buildExit = Invoke-RadioifyBuild -Context $context
        if ($buildExit -ne 0) {
            throw "Build failed with exit code $buildExit."
        }
    }

    if (-not $OutputDir) {
        $OutputDir = Join-Path $PSScriptRoot "dist\packages"
    }

    $executablePath = Resolve-RadioifyPackagingExecutablePath `
        -RepoRoot $PSScriptRoot `
        -BuildDir $context.Paths.BuildDir `
        -Config $Config
    $resolvedVersion = Get-RadioifyWindowsPackageVersion -RepoRoot $PSScriptRoot -Version $Version

    $result = New-RadioifyWindowsDistributionBundle `
        -RepoRoot $PSScriptRoot `
        -ExecutablePath $executablePath `
        -OutputRoot $OutputDir `
        -Version $resolvedVersion

    Write-Host "Windows package created:"
    Write-Host " - Bundle dir: $($result.StageDir)"
    Write-Host " - Zip: $($result.ZipPath)"
    Write-Host " - Version: $($result.Version)"
    Write-Host "Install on another PC by extracting the zip and running install_radioify.cmd."
}
catch {
    $message = $_.Exception.Message
    if (-not $message) {
        $message = $_.ToString()
    }
    Write-Error $message
    exit 1
}
