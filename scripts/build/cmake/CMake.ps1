function New-CMakeConfigureInfo {
  param([pscustomobject]$Context)

  $rootForCMake = Convert-ToCMakePath $Context.Paths.Root
  $buildDirForCMake = Convert-ToCMakePath $Context.Paths.BuildDir
  $installedRootForCMake = Convert-ToCMakePath $Context.Build.InstalledRoot
  $toolchain = $Context.Tools.Toolchain
  $options = $Context.Options
  $triplets = $Context.Build.TripletInfo

  $cmakeArgs = @("-S", $rootForCMake, "-B", $buildDirForCMake, "-Wno-deprecated")

  if ($toolchain.Generator) {
    $cmakeArgs += @("-G", $toolchain.Generator)
    if ($toolchain.NinjaExe) {
      $cmakeArgs += "-DCMAKE_MAKE_PROGRAM=$(Convert-ToCMakePath $toolchain.NinjaExe)"
    }
    if ($toolchain.Generator -eq "Ninja") {
      $cmakeArgs += "-DCMAKE_BUILD_TYPE=$($options.Config)"
    }
  }
  else {
    $cmakeArgs += "-DCMAKE_BUILD_TYPE=$($options.Config)"
  }

  if ($toolchain.ClangCl) {
    $clangClForCMake = Convert-ToCMakePath $toolchain.ClangClExe
    $cmakeArgs += "-DCMAKE_C_COMPILER=$clangClForCMake"
    $cmakeArgs += "-DCMAKE_CXX_COMPILER=$clangClForCMake"
    $cmakeArgs += "-DCMAKE_C_FLAGS_INIT=/clang:-fcolor-diagnostics /clang:-fansi-escape-codes /clang:-fcommon"
    $cmakeArgs += "-DCMAKE_CXX_FLAGS_INIT=/clang:-fcolor-diagnostics /clang:-fansi-escape-codes"
    $cmakeArgs += "-DCMAKE_RC_COMPILER=$(Convert-ToCMakePath $toolchain.ClangRcExe)"
    if ($toolchain.ClangMtExe) {
      $cmakeArgs += "-DCMAKE_MT=$(Convert-ToCMakePath $toolchain.ClangMtExe)"
    }
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
  $cmakeArgs += "-DVCPKG_MANIFEST_MODE=$desiredManifestMode"
  $cmakeArgs += "-DVCPKG_MANIFEST_INSTALL=OFF"
  if (Is-StaticTriplet $triplets.EffectiveTargetTriplet) {
    $cmakeArgs += "-DVCPKG_APPLOCAL_DEPS=OFF"
  }

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
    BuildDirForCMake = $buildDirForCMake
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
  if ($builtExe -and ($builtExe -ne $expectedExe)) {
    if (-not (Test-Path $Context.Paths.DistDir)) {
      New-Item -ItemType Directory -Force -Path $Context.Paths.DistDir | Out-Null
    }
    Copy-Item -Force -Path $builtExe -Destination $expectedExe

    $pdbSource = [System.IO.Path]::ChangeExtension($builtExe, ".pdb")
    if (Test-Path $pdbSource) {
      $pdbDest = Join-Path $Context.Paths.DistDir ([System.IO.Path]::GetFileName($pdbSource))
      Copy-Item -Force -Path $pdbSource -Destination $pdbDest
    }
  }

  if (-not (Test-Path $expectedExe)) {
    Fail-Build "Build completed without producing $expectedExe. Check the build output above."
  }

  if (Test-Path $Context.Paths.DistDir) {
    Write-Host "Build artifacts written to: $($Context.Paths.DistDir)"
    Get-ChildItem -Path $Context.Paths.DistDir -Recurse -File | ForEach-Object {
      Write-Host " - $($_.FullName)"
    }
    Write-Host "Run with: .\dist\radioify.exe <file-or-folder>"
  }
}
