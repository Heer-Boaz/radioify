function Find-FfmpegTripletDir {
  param([pscustomobject]$Context)

  $installedRoot = $Context.Build.InstalledRoot
  if (-not $installedRoot -or -not (Test-Path $installedRoot)) {
    return $null
  }

  $tripletCandidates = @()
  if ($Context.Build.TripletInfo.EffectiveTargetTriplet) { $tripletCandidates += $Context.Build.TripletInfo.EffectiveTargetTriplet }
  if ($Context.Build.TripletInfo.EffectiveDefaultTriplet) { $tripletCandidates += $Context.Build.TripletInfo.EffectiveDefaultTriplet }
  $tripletCandidates = $tripletCandidates | Select-Object -Unique

  foreach ($triplet in $tripletCandidates) {
    $candidateDir = Join-Path $installedRoot $triplet
    $ffmpegHeader = Join-Path $candidateDir "include\libavformat\avformat.h"
    if (Test-Path $ffmpegHeader) {
      return $candidateDir
    }
  }

  if ($tripletCandidates.Count -eq 0) {
    $tripletDirs = Get-ChildItem -Path $installedRoot -Directory -ErrorAction SilentlyContinue
    foreach ($dir in $tripletDirs) {
      $ffmpegHeader = Join-Path $dir.FullName "include\libavformat\avformat.h"
      if (Test-Path $ffmpegHeader) {
        return $dir.FullName
      }
    }
  }

  return $null
}

function Set-FfmpegEnvironment {
  param([string]$TripletDir)

  $ffmpegDir = Get-ProcessEnvironmentVariable -Name "FFMPEG_DIR"
  $ffmpegRoot = Get-ProcessEnvironmentVariable -Name "FFMPEG_ROOT"

  if (-not ($ffmpegDir -or $ffmpegRoot) -and $TripletDir) {
    Set-ProcessEnvironmentVariable -Name "FFMPEG_DIR" -Value (Convert-ToCMakePath $TripletDir)
    return
  }
  if ($ffmpegDir) {
    Set-ProcessEnvironmentVariable -Name "FFMPEG_DIR" -Value (Convert-ToCMakePath $ffmpegDir)
    return
  }
  if ($ffmpegRoot) {
    Set-ProcessEnvironmentVariable -Name "FFMPEG_ROOT" -Value (Convert-ToCMakePath $ffmpegRoot)
  }
}

function Assert-FfmpegAvailable {
  param(
    [pscustomobject]$Context,
    [string]$TripletDir
  )

  if ($TripletDir -or (Test-AnyEnvironmentVariableSet -Names @("FFMPEG_DIR", "FFMPEG_ROOT"))) {
    return
  }

  if ($Context.Build.TripletInfo.EffectiveTargetTriplet) {
    Fail-Build "FFmpeg not found for triplet '$($Context.Build.TripletInfo.EffectiveTargetTriplet)'. Run .\build.ps1 -InstallDeps$(if ($Context.Options.Static) { ' -Static' }) or set FFMPEG_DIR/FFMPEG_ROOT."
  }

  Fail-Build "FFmpeg not found. Run .\build.ps1 -InstallDeps (vcpkg) or set FFMPEG_DIR/FFMPEG_ROOT."
}

function Resolve-FfmpegDependency {
  param([pscustomobject]$Context)

  $ffmpegTripletDir = Find-FfmpegTripletDir -Context $Context
  Assert-FfmpegAvailable -Context $Context -TripletDir $ffmpegTripletDir
  Set-FfmpegEnvironment -TripletDir $ffmpegTripletDir
  return $ffmpegTripletDir
}
