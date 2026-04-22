function Get-BuildModuleManifest {
  return @(
    "core/Failures.ps1",
    "core/Environment.ps1",
    "core/Runtime.ps1",
    "core/NativeProcess.ps1",
    "core/Paths.ps1",
    "core/Context.ps1",
    "discovery/VisualStudio.ps1",
    "discovery/ExecutableDiscovery.ps1",
    "discovery/WindowsSdk.ps1",
    "toolchain/ClangInstall.ps1",
    "toolchain/ToolchainResolution.ps1",
    "toolchain/ToolchainEnvironment.ps1",
    "deps/Triplets.ps1",
    "deps/VcpkgDiscovery.ps1",
    "deps/VcpkgConfig.ps1",
    "deps/VcpkgInstall.ps1",
    "features/OnnxFeature.ps1",
    "features/NvidiaRtxVideoFeature.ps1",
    "features/FfmpegFeature.ps1",
    "cmake/CMake.ps1",
    "phases/CleanPhase.ps1",
    "phases/BuildToolsPhase.ps1",
    "phases/BuildDependenciesPhase.ps1",
    "phases/BuildExecutionPhase.ps1",
    "phases/BuildFlow.ps1"
  )
}

function Resolve-BuildModulePaths {
  param([string]$BuildRoot)

  $modulePaths = @()
  foreach ($moduleFile in (Get-BuildModuleManifest)) {
    $modulePath = Join-Path $BuildRoot $moduleFile
    if (-not (Test-Path $modulePath)) {
      throw "Build module is missing: $modulePath"
    }
    $modulePaths += $modulePath
  }

  return $modulePaths
}
