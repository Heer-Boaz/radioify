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

function Publish-BuildArtifacts {
  param([pscustomobject]$Context)

  $triplets = $Context.Build.TripletInfo
  $staticTriplet = $triplets.TripletStaticRequested -or (Is-StaticTriplet $triplets.EffectiveTargetTriplet) -or (Is-StaticTriplet $triplets.EffectiveDefaultTriplet)
  if (-not $Context.Options.Static -and -not $staticTriplet) {
    Copy-FfmpegRuntime -TripletDir $Context.Build.FfmpegTripletDir -Config $Context.Options.Config -DistDir $Context.Paths.DistDir
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

      $pdbSource = [System.IO.Path]::ChangeExtension($builtExe, ".pdb")
      if (Test-Path $pdbSource) {
        $pdbDest = Join-Path $Context.Paths.DistDir ([System.IO.Path]::GetFileName($pdbSource))
        $pdbPublishResult = Try-Copy-BuildArtifact -SourcePath $pdbSource -DestinationPath $pdbDest
        if (-not $pdbPublishResult.Success) {
          Write-Warning "Built PDB could not be published to dist: $($pdbPublishResult.ErrorMessage)"
        }
      }
    }
    else {
      Write-Warning "Built executable could not be published to dist: $($publishResult.ErrorMessage)"
      Write-Warning "Using build artifact directly: $builtExe"
    }
  }

  if (-not $builtExe -or -not (Test-Path $builtExe)) {
    Fail-Build "Build completed without producing a radioify executable in $($Context.Paths.BuildDir). Check the build output above."
  }

  if (Test-Path $publishedExe) {
    Write-Host "Primary executable:"
    Write-Host " - $publishedExe"
  }

  if ($publishSucceeded -and (Test-Path $Context.Paths.DistDir)) {
    Write-Host "Build artifacts written to: $($Context.Paths.DistDir)"
    Get-ChildItem -Path $Context.Paths.DistDir -Recurse -File | ForEach-Object {
      Write-Host " - $($_.FullName)"
    }
    Write-Host "Run with: .\dist\radioify.exe <file-or-folder>"
  }
  elseif ($builtExe) {
    Write-Host "Build artifacts remain in build output because dist publish was skipped or blocked."
    Write-Host "Run with: $builtExe <file-or-folder>"
  }
}
