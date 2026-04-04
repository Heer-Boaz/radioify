function Resolve-BuildToolsState {
  param([pscustomobject]$Context)

  $cmake = Resolve-CMake
  if (-not $cmake) {
    Fail-Build "CMake not found. Install CMake and ensure cmake.exe is on PATH."
  }

  $toolchain = Resolve-BuildToolchain -Options $Context.Options

  return [pscustomobject]@{
    CMake = $cmake
    Toolchain = $toolchain
    BuildDir = Join-Path $Context.Paths.Root $toolchain.BuildDirName
  }
}

function Apply-BuildToolsState {
  param(
    [pscustomobject]$Context,
    [pscustomobject]$ToolState
  )

  $Context.Tools.CMake = $ToolState.CMake
  $Context.Tools.Toolchain = $ToolState.Toolchain
  $Context.Options.Ninja = $ToolState.Toolchain.Ninja
  $Context.Options.ClangCl = $ToolState.Toolchain.ClangCl
  $Context.Paths.BuildDir = $ToolState.BuildDir
}

function Initialize-BuildTools {
  param([pscustomobject]$Context)

  $toolState = Resolve-BuildToolsState -Context $Context
  Apply-BuildToolsState -Context $Context -ToolState $toolState

  Write-Host "Using CMake: $($Context.Tools.CMake)"
  Apply-BuildToolchainEnvironment -Toolchain $Context.Tools.Toolchain
  Write-BuildToolchainSummary -Toolchain $Context.Tools.Toolchain
}
