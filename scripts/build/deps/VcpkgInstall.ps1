function Invoke-VcpkgInstall {
  param([pscustomobject]$Context)

  Assert-VcpkgRootReadableWhenRequired `
    -VcpkgRoot $Context.Tools.VcpkgRoot `
    -VcpkgExe $Context.Tools.VcpkgExe `
    -RepoRoot $Context.Paths.Root `
    -NeedsVcpkg $true

  $manifest = Join-Path $Context.Paths.Root "vcpkg.json"
  if (-not (Test-Path $manifest)) {
    Fail-Build "vcpkg.json not found. This project requires manifest mode."
  }

  $vcpkgInstallArgs = @(
    "install",
    "--clean-buildtrees-after-build",
    "--clean-packages-after-build"
  )
  if ($Context.Build.TripletInfo.EffectiveTargetTriplet) {
    $vcpkgInstallArgs += @("--triplet", $Context.Build.TripletInfo.EffectiveTargetTriplet)
  }
  if ($Context.Options.MelodyAnalysis) {
    $vcpkgInstallArgs += "--x-feature=melody-analysis"
  }

  Write-Host "Using vcpkg: $($Context.Tools.VcpkgExe)"
  $installExit = Invoke-NativeProcess -FilePath $Context.Tools.VcpkgExe -ArgumentList $vcpkgInstallArgs -WorkingDirectory $Context.Paths.Root
  if ($installExit -ne 0) {
    Write-Host ""
    Write-Host "If the error mentions a missing/invalid Visual Studio instance, install the C++ build tools (Desktop development with C++) and try again." -ForegroundColor Yellow
    Fail-Build "vcpkg install failed with exit code $installExit." -ExitCode $installExit
  }
}
