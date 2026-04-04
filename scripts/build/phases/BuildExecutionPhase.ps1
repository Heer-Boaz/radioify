function Invoke-ConfigureStep {
  param([pscustomobject]$Context)

  $Context.Build.ConfigureInfo = New-CMakeConfigureInfo -Context $Context
  Ensure-BuildCacheCompatible -Context $Context

  return (Invoke-CMakeConfigure `
      -CMake $Context.Tools.CMake `
      -ArgumentList $Context.Build.ConfigureInfo.Arguments `
      -WorkingDirectory $Context.Paths.Root `
      -BuildDir $Context.Paths.BuildDir `
      -Toolchain $Context.Tools.Toolchain)
}

function New-BuildCommandArguments {
  param([pscustomobject]$Context)

  $buildArgs = @("--build", "--preset", $Context.Tools.Toolchain.BuildPreset)
  if (-not $Context.Tools.Toolchain.Ninja) {
    $buildArgs += @("--config", $Context.Options.Config)
  }
  if ($Context.Options.VerboseBuild) {
    $buildArgs += "--verbose"
  }

  return $buildArgs
}

function Invoke-BuildStep {
  param([pscustomobject]$Context)

  $buildArgs = New-BuildCommandArguments -Context $Context
  $buildExit = Invoke-NativeProcess -FilePath $Context.Tools.CMake -ArgumentList $buildArgs -WorkingDirectory $Context.Paths.Root
  if ($buildExit -eq 0) {
    Publish-BuildArtifacts -Context $Context
  }

  return $buildExit
}
