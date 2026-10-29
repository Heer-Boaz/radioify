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
    @{ Source = "scripts/windows/RadioifyWindowsPackageFiles.ps1"; Destination = "scripts/windows/RadioifyWindowsPackageFiles.ps1" },
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

    [void](Copy-RadioifyFileIfChanged -Source $sourcePath -Destination $destinationPath)
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

    # Keep the signed package, certificate and manifest in one snapshot while
    # another export or build may be waiting to replace them.
    $lock = Enter-RadioifyPackageLock -Directory $integrationDir -Name ".msix-build.lock"
    try {
        $packageResult = Invoke-RadioifySignedMsixPackageBuild -IntegrationDir $integrationDir

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

            [void](Copy-RadioifyFileIfChanged -Source $sourcePath -Destination $destinationPath)
        }

        foreach ($sourcePath in @($packageResult.PackagePath, $packageResult.CertificatePath)) {
            if (-not (Test-Path -LiteralPath $sourcePath)) {
                throw "Win11 Explorer integration package artifact was not created: '$sourcePath'."
            }

            $destinationPath = Join-Path $StageDir ([System.IO.Path]::GetFileName($sourcePath))
            [void](Copy-RadioifyFileIfChanged -Source $sourcePath -Destination $destinationPath)
        }
    } finally { $lock.Dispose() }
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

    $OutputRoot = (Get-Item -LiteralPath $OutputRoot).FullName
    $lock = Enter-RadioifyPackageLock -Directory $OutputRoot -Name ".distribution-build.lock"
    try {
        $publishedStage = Join-Path $OutputRoot $script:RadioifyWindowsBundleName
        $stageDir = Join-Path $OutputRoot ".$($script:RadioifyWindowsBundleName).building"
        $backupStage = Join-Path $OutputRoot ".$($script:RadioifyWindowsBundleName).previous"
        $zipPath = Join-Path $OutputRoot "$script:RadioifyWindowsBundleName.zip"
        $pendingZip = "$zipPath.building.zip"

        # Recover an interrupted directory swap before preparing a new export.
        if (Test-Path -LiteralPath $backupStage) {
            Get-RadioifyOwnedBundleItems -Path $backupStage -ExpectedParent $OutputRoot | Out-Null
            if (-not (Test-Path -LiteralPath $publishedStage)) {
                [IO.Directory]::Move($backupStage, $publishedStage)
            } else {
                Get-RadioifyOwnedBundleItems -Path $publishedStage -ExpectedParent $OutputRoot | Out-Null
                Remove-RadioifyOwnedBundleDirectory -Path $backupStage -ExpectedParent $OutputRoot
            }
        }
        Remove-RadioifyOwnedBundleDirectory -Path $stageDir -ExpectedParent $OutputRoot
        if ([IO.File]::Exists($pendingZip)) { [IO.File]::Delete($pendingZip) }

        New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
        $manifest = New-RadioifyWindowsPackageManifest -Version $Version -ExecutableName "radioify.exe"
        Write-RadioifyPackageState -Path (Join-Path $stageDir "RadioifyPackage.json") -State $manifest
        try {

            [void](Copy-RadioifyFileIfChanged -Source $resolvedExecutablePath -Destination (Join-Path $stageDir "radioify.exe"))
            $llamaLicensePath = Join-Path $resolvedRepoRoot "dist\llama-cpp-LICENSE.txt"
            if (-not (Test-Path -LiteralPath $llamaLicensePath)) {
                throw "llama.cpp license not found at '$llamaLicensePath'. Run .\build.ps1 -Static first."
            }
            [void](Copy-RadioifyFileIfChanged -Source $llamaLicensePath -Destination (Join-Path $stageDir "llama-cpp-LICENSE.txt"))

            $iconPath = Join-Path $resolvedRepoRoot "radioify.ico"
            if (Test-Path -LiteralPath $iconPath) {
                [void](Copy-RadioifyFileIfChanged -Source $iconPath -Destination (Join-Path $stageDir "radioify.ico"))
            }

            $modelStageDir = Join-Path $stageDir "models"
            New-Item -ItemType Directory -Force -Path $modelStageDir | Out-Null

            $windowsMlRuntimeContract = Get-RadioifyWindowsMlRuntimeContract
            Copy-RadioifyTranscriptBootstrap -Source (Join-Path $resolvedRepoRoot "dist\transcript") `
                -Destination (Join-Path $stageDir "transcript")
            foreach ($runtimeName in $windowsMlRuntimeContract.ProductionRuntimeFiles) {
                $runtimeSource = Join-Path $resolvedRepoRoot "dist\$runtimeName"
                if (-not (Test-Path -LiteralPath $runtimeSource)) {
                    throw "Audio-separation runtime not found at '$runtimeSource'. Run .\build.ps1 -Static first."
                }
                [void](Copy-RadioifyFileIfChanged -Source $runtimeSource -Destination (Join-Path $stageDir $runtimeName))
            }

            $separationSourceDir = Join-Path $resolvedRepoRoot "dist\models\audio_separation"
            foreach ($separationFile in @(
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
                [void](Copy-RadioifyFileIfChanged -Source $sourcePath -Destination (Join-Path $destinationDir $separationFile))
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

            Write-Host "Creating distribution ZIP..."
            New-RadioifyDistributionZip -StageDir $stageDir -ArchivePath $pendingZip -RootName $script:RadioifyWindowsBundleName
            if (Test-Path -LiteralPath $publishedStage) {
                Get-RadioifyOwnedBundleItems -Path $publishedStage -ExpectedParent $OutputRoot | Out-Null
                [IO.Directory]::Move($publishedStage, $backupStage)
            }
            try {
                [IO.Directory]::Move($stageDir, $publishedStage)
                Publish-RadioifyPackageFile -PendingPath $pendingZip -Destination $zipPath
            } catch {
                if (Test-Path -LiteralPath $publishedStage) {
                    Remove-RadioifyOwnedBundleDirectory -Path $publishedStage -ExpectedParent $OutputRoot
                }
                if (Test-Path -LiteralPath $backupStage) { [IO.Directory]::Move($backupStage, $publishedStage) }
                throw
            }
            Remove-RadioifyOwnedBundleDirectory -Path $backupStage -ExpectedParent $OutputRoot

            return [pscustomobject]@{
                StageDir = $publishedStage
                ZipPath = $zipPath
                Version = $Version
                ExecutablePath = Join-Path $publishedStage "radioify.exe"
            }
        } finally {
            Remove-RadioifyOwnedBundleDirectory -Path $stageDir -ExpectedParent $OutputRoot
            if ([IO.File]::Exists($pendingZip)) { [IO.File]::Delete($pendingZip) }
        }
    } finally { $lock.Dispose() }
}
