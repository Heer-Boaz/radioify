function Test-BuildNeedsVcpkg {
  param([string]$InstalledRoot)

  $manifestInstalledAvailable = [bool]($InstalledRoot -and (Test-Path $InstalledRoot))
  return (-not (Test-AnyEnvironmentVariableSet -Names @("FFMPEG_DIR", "FFMPEG_ROOT")) -and -not $manifestInstalledAvailable)
}

function Resolve-BuildDependencyState {
  param([pscustomobject]$Context)

  return [pscustomobject]@{
    VcpkgRoot = Resolve-VcpkgRoot -PreferredRoot $Context.Options.VcpkgRoot
    VcpkgExe = $null
    TripletInfo = Set-BuildTripletEnvironment -Options $Context.Options
  }
}

function Apply-BuildDependencyState {
  param(
    [pscustomobject]$Context,
    [pscustomobject]$DependencyState
  )

  $Context.Tools.VcpkgRoot = $DependencyState.VcpkgRoot
  $Context.Tools.VcpkgExe = $DependencyState.VcpkgExe
  $Context.Build.TripletInfo = $DependencyState.TripletInfo
}

function Initialize-BuildDependencies {
  param([pscustomobject]$Context)

  Assert-ExplicitVcpkgRootValid -ProvidedRoot $Context.Options.VcpkgRoot
  Configure-MelodyAnalysisBuild -Context $Context

  $dependencyState = Resolve-BuildDependencyState -Context $Context
  $dependencyState.VcpkgExe = Resolve-VcpkgExe -VcpkgRoot $dependencyState.VcpkgRoot

  Apply-BuildDependencyState -Context $Context -DependencyState $dependencyState

  if ($Context.Options.InstallDeps) {
    Invoke-VcpkgInstall -Context $Context
  }

  $Context.Build.InstalledRoot = Resolve-InstalledRoot -Context $Context
  $needsVcpkg = Test-BuildNeedsVcpkg -InstalledRoot $Context.Build.InstalledRoot

  Assert-VcpkgRootReadableWhenRequired `
    -VcpkgRoot $Context.Tools.VcpkgRoot `
    -VcpkgExe $Context.Tools.VcpkgExe `
    -RepoRoot $Context.Paths.Root `
    -NeedsVcpkg $needsVcpkg

  if ($Context.Options.MelodyAnalysis -and (Is-StaticTriplet $Context.Build.TripletInfo.EffectiveTargetTriplet)) {
    Assert-OnnxStaticRegistrationDisabled `
      -InstalledRoot $Context.Build.InstalledRoot `
      -Triplet $Context.Build.TripletInfo.EffectiveTargetTriplet
  }

  $Context.Build.FfmpegTripletDir = Resolve-FfmpegDependency -Context $Context
}
