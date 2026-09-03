Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "RadioifyWindowsMsixBuild.ps1")

$script:RadioifyWindowsBundleName = "Radioify-Windows-x64"
$script:RadioifyWindowsBundleFiles = @(
    @{ Source = "scripts/windows/RadioifyWindowsShellCommon.ps1"; Destination = "scripts/windows/RadioifyWindowsShellCommon.ps1" },
    @{ Source = "scripts/windows/RadioifyWindowsExplorerIntegrationHost.ps1"; Destination = "scripts/windows/RadioifyWindowsExplorerIntegrationHost.ps1" },
    @{ Source = "scripts/windows/RadioifyWindowsBundleMsixInstall.ps1"; Destination = "scripts/windows/RadioifyWindowsBundleMsixInstall.ps1" },
    @{ Source = "scripts/windows/RadioifyWindowsMsixCommon.ps1"; Destination = "scripts/windows/RadioifyWindowsMsixCommon.ps1" },
    @{ Source = "scripts/windows/RadioifyWindowsMsixBuild.ps1"; Destination = "scripts/windows/RadioifyWindowsMsixBuild.ps1" },
    @{ Source = "scripts/windows/RadioifyWindowsRuntimeContract.ps1"; Destination = "scripts/windows/RadioifyWindowsRuntimeContract.ps1" },
    @{ Source = "scripts/windows/RadioifyWindowsMsixInstall.ps1"; Destination = "scripts/windows/RadioifyWindowsMsixInstall.ps1" },
    @{ Source = "scripts/windows/install_radioify_windows_bundle.ps1"; Destination = "scripts/windows/install_radioify_windows_bundle.ps1" },
    @{ Source = "scripts/windows/uninstall_radioify_windows_bundle.ps1"; Destination = "scripts/windows/uninstall_radioify_windows_bundle.ps1" },
    @{ Source = "scripts/windows/install_radioify_msix_package.ps1"; Destination = "scripts/windows/install_radioify_msix_package.ps1" },
    @{ Source = "scripts/windows/uninstall_radioify_msix_package.ps1"; Destination = "scripts/windows/uninstall_radioify_msix_package.ps1" },
    @{ Source = "windows/distribution/install_radioify.ps1"; Destination = "install_radioify.ps1" },
    @{ Source = "windows/distribution/uninstall_radioify.ps1"; Destination = "uninstall_radioify.ps1" },
    @{ Source = "windows/distribution/install_radioify.cmd"; Destination = "install_radioify.cmd" },
    @{ Source = "windows/distribution/uninstall_radioify.cmd"; Destination = "uninstall_radioify.cmd" },
    @{ Source = "windows/distribution/README.txt.in"; Destination = "README.txt" }
)

$script:RadioifyWindowsExplorerIntegrationBundleFiles = @(
    @{ Source = "Package.appxmanifest"; Destination = "win11-explorer-integration/Package.appxmanifest" },
    @{ Source = "radioify_explorer.dll"; Destination = "win11-explorer-integration/radioify_explorer.dll" },
    @{ Source = "README.txt"; Destination = "win11-explorer-integration/README.txt" }
)

function Get-RadioifyWindowsPackageVersion {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [string]$Version
    )

    if ($Version) {
        return $Version
    }

    $stamp = Get-Date -Format "yyyy.MM.dd"
    $gitCommit = $null
    try {
        $gitCommit = (& git -C $RepoRoot rev-parse --short HEAD 2>$null | Select-Object -First 1)
    } catch {
        $gitCommit = $null
    }

    if ($gitCommit) {
        return "$stamp+$gitCommit"
    }

    return $stamp
}

function Copy-RadioifyWindowsPackageFile {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$StageDir,
        [Parameter(Mandatory = $true)][string]$SourceRelativePath,
        [Parameter(Mandatory = $true)][string]$DestinationRelativePath
    )

    $sourcePath = Join-Path $RepoRoot $SourceRelativePath
    if (-not (Test-Path -LiteralPath $sourcePath)) {
        throw "Package source file not found: '$sourcePath'."
    }

    $destinationPath = Join-Path $StageDir $DestinationRelativePath
    $destinationDir = Split-Path -Parent $destinationPath
    if ($destinationDir -and -not (Test-Path -LiteralPath $destinationDir)) {
        New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null
    }

    Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force
}

function Copy-RadioifyWindowsExplorerIntegrationBundle {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$StageDir
    )

    $integrationDir = Join-Path $RepoRoot "dist\win11-explorer-integration"
    if (-not (Test-Path -LiteralPath $integrationDir)) {
        throw "Win11 Explorer integration artifacts were not found at '$integrationDir'. Run .\build.ps1 -Static -Win11ExplorerIntegration or rerun .\package_windows.ps1 without -SkipBuild."
    }

    $packageResult = New-RadioifySignedMsixPackage -IntegrationDir $integrationDir

    foreach ($file in $script:RadioifyWindowsExplorerIntegrationBundleFiles) {
        $sourcePath = Join-Path $integrationDir $file.Source
        if (-not (Test-Path -LiteralPath $sourcePath)) {
            throw "Win11 Explorer integration bundle source file not found: '$sourcePath'."
        }

        $destinationPath = Join-Path $StageDir $file.Destination
        $destinationDir = Split-Path -Parent $destinationPath
        if ($destinationDir -and -not (Test-Path -LiteralPath $destinationDir)) {
            New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null
        }

        Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force
    }

    foreach ($sourcePath in @($packageResult.PackagePath, $packageResult.CertificatePath)) {
        if (-not (Test-Path -LiteralPath $sourcePath)) {
            throw "Win11 Explorer integration package artifact was not created: '$sourcePath'."
        }

        $destinationPath = Join-Path $StageDir ([System.IO.Path]::GetFileName($sourcePath))
        Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force
    }
}

function New-RadioifyWindowsPackageManifest {
    param(
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$ExecutableName
    )

    return [ordered]@{
        PackageFormatVersion = 1
        DisplayName = "Radioify"
        DisplayVersion = $Version
        ExecutableName = $ExecutableName
        Architecture = "x64"
        BuiltAtUtc = [DateTime]::UtcNow.ToString("o")
    }
}

function New-RadioifyWindowsDistributionBundle {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$ExecutablePath,
        [Parameter(Mandatory = $true)][string]$OutputRoot,
        [Parameter(Mandatory = $true)][string]$Version
    )

    $resolvedRepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
    $resolvedExecutablePath = (Resolve-Path -LiteralPath $ExecutablePath).Path

    if (-not (Test-Path -LiteralPath $resolvedExecutablePath)) {
        throw "Packaged executable not found at '$resolvedExecutablePath'."
    }

    if (-not (Test-Path -LiteralPath $OutputRoot)) {
        New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
    }

    $stageDir = Join-Path $OutputRoot $script:RadioifyWindowsBundleName
    $zipPath = Join-Path $OutputRoot "$script:RadioifyWindowsBundleName.zip"

    if (Test-Path -LiteralPath $stageDir) {
        Remove-Item -LiteralPath $stageDir -Recurse -Force
    }
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }

    New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

    Copy-Item -LiteralPath $resolvedExecutablePath -Destination (Join-Path $stageDir "radioify.exe") -Force
    $chapterWorkerPath = Join-Path $resolvedRepoRoot "dist\radioify_chapter_worker.exe"
    if (-not (Test-Path -LiteralPath $chapterWorkerPath -PathType Leaf)) {
        throw "Isolated chapter worker not found at '$chapterWorkerPath'. Run .\build.ps1 -Static first."
    }
    Copy-Item -LiteralPath $chapterWorkerPath `
        -Destination (Join-Path $stageDir "radioify_chapter_worker.exe") `
        -Force

    $llamaLicensePath = Join-Path $resolvedRepoRoot "dist\llama-cpp-LICENSE.txt"
    if (-not (Test-Path -LiteralPath $llamaLicensePath)) {
        throw "llama.cpp license not found at '$llamaLicensePath'. Run .\build.ps1 -Static first."
    }
    Copy-Item -LiteralPath $llamaLicensePath `
        -Destination (Join-Path $stageDir "llama-cpp-LICENSE.txt") `
        -Force

    $iconPath = Join-Path $resolvedRepoRoot "radioify.ico"
    if (Test-Path -LiteralPath $iconPath) {
        Copy-Item -LiteralPath $iconPath -Destination (Join-Path $stageDir "radioify.ico") -Force
    }

    $whisperModelPath = Join-Path $resolvedRepoRoot "dist\models\ggml-base-q5_1.bin"
    if (-not (Test-Path -LiteralPath $whisperModelPath)) {
        throw "Indexed-transcript model not found at '$whisperModelPath'. Run .\build.ps1 -Static first."
    }
    $modelStageDir = Join-Path $stageDir "models"
    New-Item -ItemType Directory -Force -Path $modelStageDir | Out-Null
    Copy-Item -LiteralPath $whisperModelPath `
        -Destination (Join-Path $modelStageDir "ggml-base-q5_1.bin") `
        -Force

    $chapterPlannerSourceDir = Join-Path $resolvedRepoRoot "dist\models\chapter_analysis"
    $chapterPlannerStageDir = Join-Path $modelStageDir "chapter_analysis"
    New-Item -ItemType Directory -Force -Path $chapterPlannerStageDir | Out-Null
    foreach ($chapterPlannerFile in @(
        "chapter-llama-captions-asr-10k-f16.gguf",
        "CHAPTER-LLAMA-NOTICE.md",
        "LLAMA-3.1-LICENSE",
        "NOTICE"
    )) {
        $sourcePath = Join-Path $chapterPlannerSourceDir $chapterPlannerFile
        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
            throw "Chapter-Llama planner asset not found at '$sourcePath'. Run .\build.ps1 -Static first."
        }
        Copy-Item -LiteralPath $sourcePath `
            -Destination (Join-Path $chapterPlannerStageDir $chapterPlannerFile) `
            -Force
    }

    $windowsMlRuntimeContract = Get-RadioifyWindowsMlRuntimeContract
    foreach ($runtimeName in $windowsMlRuntimeContract.ProductionRuntimeFiles) {
        $runtimeSource = Join-Path $resolvedRepoRoot "dist\$runtimeName"
        if (-not (Test-Path -LiteralPath $runtimeSource)) {
            throw "Audio-separation runtime not found at '$runtimeSource'. Run .\build.ps1 -Static first."
        }
        Copy-Item -LiteralPath $runtimeSource `
            -Destination (Join-Path $stageDir $runtimeName) `
            -Force
    }

    $separationSourceDir = Join-Path $resolvedRepoRoot "dist\models\audio_separation"
    foreach ($separationFile in @(
        "bandit-v2-multi-mask-core-fp16.onnx",
        "BandIt-v2-MODEL-CARD"
    )) {
        $sourcePath = Join-Path $separationSourceDir $separationFile
        if (-not (Test-Path -LiteralPath $sourcePath)) {
            throw "Audio-separation artifact not found at '$sourcePath'. Run .\build.ps1 -Static first."
        }
        $destinationDir = Join-Path $modelStageDir "audio_separation"
        if (-not (Test-Path -LiteralPath $destinationDir)) {
            New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null
        }
        Copy-Item -LiteralPath $sourcePath `
            -Destination (Join-Path $destinationDir $separationFile) `
            -Force
    }

    foreach ($file in $script:RadioifyWindowsBundleFiles) {
        Copy-RadioifyWindowsPackageFile `
            -RepoRoot $resolvedRepoRoot `
            -StageDir $stageDir `
            -SourceRelativePath $file.Source `
            -DestinationRelativePath $file.Destination
    }

    Copy-RadioifyWindowsExplorerIntegrationBundle `
        -RepoRoot $resolvedRepoRoot `
        -StageDir $stageDir

    $manifest = New-RadioifyWindowsPackageManifest -Version $Version -ExecutableName "radioify.exe"
    $manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $stageDir "RadioifyPackage.json") -Encoding UTF8

    Compress-Archive -LiteralPath $stageDir -DestinationPath $zipPath -CompressionLevel Optimal

    return [pscustomobject]@{
        StageDir = $stageDir
        ZipPath = $zipPath
        Version = $Version
        ExecutablePath = Join-Path $stageDir "radioify.exe"
    }
}
