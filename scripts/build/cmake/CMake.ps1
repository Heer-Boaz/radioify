function New-CMakeConfigureInfo {
  param([pscustomobject]$Context)

  $installedRootForCMake = Convert-ToCMakePath $Context.Build.InstalledRoot
  $toolchain = $Context.Tools.Toolchain
  $options = $Context.Options
  $triplets = $Context.Build.TripletInfo

  $cmakeArgs = @("--preset", $toolchain.ConfigurePreset, "-Wno-deprecated")

  if ($toolchain.Generator -eq "Ninja" -or -not $toolchain.Generator) {
    $cmakeArgs += "-DCMAKE_BUILD_TYPE=$($options.Config)"
  }

  if ($toolchain.WindowsFxcExe) {
    $cmakeArgs += "-DRADIOIFY_FXC_EXE=$(Convert-ToCMakePath $toolchain.WindowsFxcExe)"
  }

  $cmakeArgs += "-DRADIOIFY_ENABLE_TIMING_LOG=$([bool]$options.TimingLog)"
  $cmakeArgs += "-DRADIOIFY_ENABLE_STAGING_UPLOAD=$([bool]$options.StagingUpload)"
  $cmakeArgs += "-DRADIOIFY_ENABLE_VIDEO_ERROR_LOG=$([bool]$options.VideoErrorLog)"
  $cmakeArgs += "-DRADIOIFY_ENABLE_FFMPEG_ERROR_LOG=$([bool]$options.FfmpegErrorLog)"
  $cmakeArgs += "-DRADIOIFY_ENABLE_MELODY_ANALYSIS=$([bool]$options.MelodyAnalysis)"
  $cmakeArgs += "-DRADIOIFY_ENABLE_NEURAL_PITCH=$([bool]$options.MelodyAnalysis)"
  $cmakeArgs += "-DRADIOIFY_BUILD_WIN11_EXPLORER_INTEGRATION=$([bool]$options.Win11ExplorerIntegration)"
  $cmakeArgs += "-DRADIOIFY_ENABLE_NVIDIA_RTX_VIDEO_SDK=$([bool]$options.NvidiaRtxVideo)"
  if ($Context.Build.NvidiaRtxVideoSdkRoot) {
    $cmakeArgs += "-DRADIOIFY_NVIDIA_RTX_VIDEO_SDK_ROOT=$(Convert-ToCMakePath $Context.Build.NvidiaRtxVideoSdkRoot)"
  }

  $desiredManifestMode = "OFF"

  $desiredToolchainForCache = $null
  if ($Context.Tools.VcpkgRoot) {
    $toolchainPath = Join-Path $Context.Tools.VcpkgRoot "scripts/buildsystems/vcpkg.cmake"
    if (Test-Path $toolchainPath) {
      $desiredToolchainForCache = Convert-ToCMakePath $toolchainPath
      $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$desiredToolchainForCache"
    }
  }

  if ($Context.Build.InstalledRoot) {
    $cmakeArgs += "-DVCPKG_INSTALLED_DIR=$installedRootForCMake"
  }
  if ($triplets.EffectiveTargetTriplet) {
    $cmakeArgs += "-DVCPKG_TARGET_TRIPLET=$($triplets.EffectiveTargetTriplet)"
  }
  if ($env:VCPKG_OVERLAY_PORTS) {
    $cmakeArgs += "-DVCPKG_OVERLAY_PORTS=$(Convert-ToCMakePathList $env:VCPKG_OVERLAY_PORTS)"
  }

  return [pscustomobject]@{
    Arguments = $cmakeArgs
    DesiredManifestMode = $desiredManifestMode
    DesiredToolchainForCache = $desiredToolchainForCache
  }
}

function Test-Win11ExplorerIntegrationPackage {
  param(
    [Parameter(Mandatory = $true)]
    [pscustomobject]$Context,
    [Parameter(Mandatory = $true)]
    [string]$IntegrationDistDir
  )

  $packageCommonScript = Join-Path $Context.Paths.Root "scripts\windows\RadioifyWin11PackageCommon.ps1"
  if (-not (Test-Path -LiteralPath $packageCommonScript)) {
    Fail-Build "Win11 Explorer integration package validation script is missing: $packageCommonScript"
  }

  . $packageCommonScript

  $packageLayout = Initialize-RadioifyWin11PackageLayout -IntegrationDir $IntegrationDistDir
  $manifestInfo = Get-RadioifyWin11ManifestInfo -IntegrationDir $IntegrationDistDir
  $makeAppxExe = Resolve-WindowsSdkExecutable -ToolName "makeappx.exe"
  $validationPackagePath = Join-Path $IntegrationDistDir "$($manifestInfo.PackageName).validation.msix"
  if (Test-Path -LiteralPath $validationPackagePath) {
    Remove-Item -LiteralPath $validationPackagePath -Force
  }

  $makeAppxExit = Invoke-NativeProcessWithOutput -FilePath $makeAppxExe -ArgumentList @(
    "pack",
    "/d", $packageLayout,
    "/p", $validationPackagePath,
    "/o",
    "/nv"
  ) -WorkingDirectory $Context.Paths.Root

  if ($makeAppxExit -ne 0) {
    Fail-Build "Win11 Explorer integration package validation failed while running makeappx.exe."
  }

  if (-not (Test-Path -LiteralPath $validationPackagePath)) {
    Fail-Build "Win11 Explorer integration package validation did not produce $validationPackagePath"
  }

  Remove-Item -LiteralPath $validationPackagePath -Force
}

function Add-PublishedArtifactPath {
  param(
    [System.Collections.Generic.List[string]]$Artifacts,
    [string]$Path
  )

  if ($null -eq $Artifacts -or -not $Path -or -not (Test-Path -LiteralPath $Path)) {
    return
  }

  $fullPath = (Get-Item -LiteralPath $Path).FullName
  if (-not $Artifacts.Contains($fullPath)) {
    [void]$Artifacts.Add($fullPath)
  }
}

function Add-PublishedArtifactsFromDirectory {
  param(
    [System.Collections.Generic.List[string]]$Artifacts,
    [string]$DirectoryPath
  )

  if ($null -eq $Artifacts -or -not $DirectoryPath -or -not (Test-Path -LiteralPath $DirectoryPath)) {
    return
  }

  Get-ChildItem -LiteralPath $DirectoryPath -Recurse -File |
    Sort-Object FullName |
    ForEach-Object {
      Add-PublishedArtifactPath -Artifacts $Artifacts -Path $_.FullName
    }
}

function Ensure-BuildCacheCompatible {
  param([pscustomobject]$Context)

  $buildDir = $Context.Paths.BuildDir
  $cachePath = Join-Path $buildDir "CMakeCache.txt"
  $cachedManifestMode = Get-CMakeCacheValue -CachePath $cachePath -Variable "VCPKG_MANIFEST_MODE"
  $initialManifestMode = Get-CMakeCacheValue -CachePath $cachePath -Variable "Z_VCPKG_CHECK_MANIFEST_MODE"
  $cachedToolchainFile = Get-CMakeCacheValue -CachePath $cachePath -Variable "CMAKE_TOOLCHAIN_FILE"

  $effectiveCachedManifestMode = $null
  if ($initialManifestMode) {
    $effectiveCachedManifestMode = $initialManifestMode
  }
  elseif ($cachedManifestMode) {
    $effectiveCachedManifestMode = $cachedManifestMode
  }

  if ($effectiveCachedManifestMode -and ($effectiveCachedManifestMode.ToUpperInvariant() -ne $Context.Build.ConfigureInfo.DesiredManifestMode)) {
    Write-Host "Detected VCPKG_MANIFEST_MODE mismatch in build cache: $effectiveCachedManifestMode -> $($Context.Build.ConfigureInfo.DesiredManifestMode)"
    Write-Host "Recreating build directory to avoid unsupported cache transition."
    if (Test-Path $buildDir) {
      Remove-PathWithRetry -Path $buildDir
    }
    return
  }

  if ($Context.Build.ConfigureInfo.DesiredToolchainForCache -and $cachedToolchainFile -and ($cachedToolchainFile -ne $Context.Build.ConfigureInfo.DesiredToolchainForCache)) {
    Write-Host "Detected CMAKE_TOOLCHAIN_FILE mismatch in build cache:"
    Write-Host "  cached : $cachedToolchainFile"
    Write-Host "  desired: $($Context.Build.ConfigureInfo.DesiredToolchainForCache)"
    Write-Host "Recreating build directory to clear stale broken CMake cache."
    if (Test-Path $buildDir) {
      Remove-PathWithRetry -Path $buildDir
    }
  }
}

function Test-BuildArtifactPublishLock {
  param([string]$ErrorMessage)

  if (-not $ErrorMessage) {
    return $false
  }
  return $ErrorMessage.IndexOf("being used by another process", [System.StringComparison]::OrdinalIgnoreCase) -ge 0
}

function Publish-BuildArtifacts {
  param([pscustomobject]$Context)

  $publishedArtifacts = New-Object System.Collections.Generic.List[string]
  $triplets = $Context.Build.TripletInfo
  $staticTriplet = $triplets.TripletStaticRequested -or (Is-StaticTriplet $triplets.EffectiveTargetTriplet) -or (Is-StaticTriplet $triplets.EffectiveDefaultTriplet)
  if (-not $Context.Options.Static -and -not $staticTriplet) {
    $ffmpegRuntimeArtifacts = Copy-FfmpegRuntime -TripletDir $Context.Build.FfmpegTripletDir -Config $Context.Options.Config -DistDir $Context.Paths.DistDir
    foreach ($artifact in $ffmpegRuntimeArtifacts) {
      Add-PublishedArtifactPath -Artifacts $publishedArtifacts -Path $artifact
    }
  }
  if ($Context.Options.NvidiaRtxVideo) {
    $nvidiaRuntimeArtifacts = Copy-NvidiaRtxVideoRuntime `
      -SdkRoot $Context.Build.NvidiaRtxVideoSdkRoot `
      -DistDir $Context.Paths.DistDir
    foreach ($artifact in $nvidiaRuntimeArtifacts) {
      Add-PublishedArtifactPath -Artifacts $publishedArtifacts -Path $artifact
    }
  }

  $expectedExe = Join-Path $Context.Paths.DistDir "radioify.exe"
  $builtExe = Find-RadioifyExecutable -Root $Context.Paths.Root -BuildDir $Context.Paths.BuildDir -Config $Context.Options.Config
  $publishedExe = $builtExe
  $publishSucceeded = ($builtExe -and ($builtExe -eq $expectedExe))
  if ($builtExe -and ($builtExe -ne $expectedExe)) {
    $publishResult = Try-Copy-BuildArtifact -SourcePath $builtExe -DestinationPath $expectedExe
    if ($publishResult.Success) {
      $publishedExe = $expectedExe
      $publishSucceeded = $true
      Add-PublishedArtifactPath -Artifacts $publishedArtifacts -Path $publishedExe

      $pdbSource = [System.IO.Path]::ChangeExtension($builtExe, ".pdb")
      if (Test-Path $pdbSource) {
        $pdbDest = Join-Path $Context.Paths.DistDir ([System.IO.Path]::GetFileName($pdbSource))
        $pdbPublishResult = Try-Copy-BuildArtifact -SourcePath $pdbSource -DestinationPath $pdbDest
        if (-not $pdbPublishResult.Success) {
          Write-Warning "Built PDB could not be published to dist: $($pdbPublishResult.ErrorMessage)"
        } else {
          Add-PublishedArtifactPath -Artifacts $publishedArtifacts -Path $pdbDest
        }
      }
    }
    else {
      if (Test-BuildArtifactPublishLock -ErrorMessage $publishResult.ErrorMessage) {
        Write-Host "Dist publish skipped because the existing executable is in use: $($publishResult.ErrorMessage)"
        Write-Host "Using build artifact directly: $builtExe"
      }
      else {
        Write-Warning "Built executable could not be published to dist: $($publishResult.ErrorMessage)"
        Write-Warning "Using build artifact directly: $builtExe"
      }
    }
  }

  if (-not $builtExe -or -not (Test-Path $builtExe)) {
    Fail-Build "Build completed without producing a radioify executable in $($Context.Paths.BuildDir). Check the build output above."
  }

  # Copy radioify.ico to dist/ so it ends up in the Win11 Explorer external location.
  $icoSource = Join-Path $Context.Paths.Root "radioify.ico"
  if (Test-Path $icoSource) {
    $icoDest = Join-Path $Context.Paths.DistDir "radioify.ico"
    Copy-Item -LiteralPath $icoSource -Destination $icoDest -Force
    Add-PublishedArtifactPath -Artifacts $publishedArtifacts -Path $icoDest
  }

  if (Test-Path $publishedExe) {
    Add-PublishedArtifactPath -Artifacts $publishedArtifacts -Path $publishedExe
    Write-Host "Primary executable:"
    Write-Host " - $publishedExe"
  }

  if ($Context.Options.Win11ExplorerIntegration) {
    $integrationDistDir = Join-Path $Context.Paths.DistDir "win11-explorer-integration"
    if (-not (Test-Path $integrationDistDir)) {
      New-Item -ItemType Directory -Force -Path $integrationDistDir | Out-Null
    }

    foreach ($publishedName in @(
        "radioify_explorer.dll",
        "radioify_explorer.pdb",
        "Package.appxmanifest",
        "README.txt"
      )) {
      $publishedPath = Join-Path $integrationDistDir $publishedName
      if (Test-Path -LiteralPath $publishedPath) {
        Remove-Item -LiteralPath $publishedPath -Force
      }
    }

    $explorerDll = Find-RadioifyExplorerDll -BuildDir $Context.Paths.BuildDir -Config $Context.Options.Config
    if (-not $explorerDll) {
      Fail-Build "Win11 Explorer integration build completed without producing radioify_explorer.dll."
    }

    $dllPublishResult = Try-Copy-BuildArtifact `
      -SourcePath $explorerDll `
      -DestinationPath (Join-Path $integrationDistDir "radioify_explorer.dll")
    if (-not $dllPublishResult.Success) {
      Fail-Build "Failed to publish radioify_explorer.dll: $($dllPublishResult.ErrorMessage)"
    }

    $explorerPdb = [System.IO.Path]::ChangeExtension($explorerDll, ".pdb")
    if (Test-Path $explorerPdb) {
      $pdbPublishResult = Try-Copy-BuildArtifact `
        -SourcePath $explorerPdb `
        -DestinationPath (Join-Path $integrationDistDir ([System.IO.Path]::GetFileName($explorerPdb)))
      if (-not $pdbPublishResult.Success) {
        Write-Warning "Win11 Explorer integration PDB could not be published: $($pdbPublishResult.ErrorMessage)"
      }
    }

    foreach ($asset in @(
        (Find-RadioifyWin11PackageManifest -BuildDir $Context.Paths.BuildDir),
        (Find-RadioifyWin11PackageReadme -BuildDir $Context.Paths.BuildDir)
      )) {
      if (-not $asset) { continue }
      if (-not (Test-Path $asset)) { continue }
      $assetPublishResult = Try-Copy-BuildArtifact `
        -SourcePath $asset `
        -DestinationPath (Join-Path $integrationDistDir ([System.IO.Path]::GetFileName($asset)))
      if (-not $assetPublishResult.Success) {
        Fail-Build "Failed to publish Win11 Explorer integration asset '$asset': $($assetPublishResult.ErrorMessage)"
      }
    }

    Test-Win11ExplorerIntegrationPackage -Context $Context -IntegrationDistDir $integrationDistDir
    Add-PublishedArtifactsFromDirectory -Artifacts $publishedArtifacts -DirectoryPath $integrationDistDir

    Write-Host "Win11 Explorer integration scaffold:"
    Get-ChildItem -Path $integrationDistDir -File | Sort-Object Name | ForEach-Object {
      Write-Host " - $($_.FullName)"
    }
  }

  if ($publishSucceeded -and (Test-Path $Context.Paths.DistDir)) {
    Write-Host "Build artifacts written to: $($Context.Paths.DistDir)"
    foreach ($artifact in $publishedArtifacts) {
      Write-Host " - $artifact"
    }
    Write-Host "Run with: .\dist\radioify.exe <file-or-folder>"
  }
  elseif ($builtExe) {
    Write-Host "Build artifacts remain in build output because dist publish was skipped or blocked."
    Write-Host "Run with: $builtExe <file-or-folder>"
  }
}
