function New-BuildToolchainState {
  param([pscustomobject]$Options)

  return [ordered]@{
    Ninja            = [bool]$Options.Ninja
    NinjaExe         = $null
    Generator        = $null
    ClangCl          = [bool]$Options.ClangCl
    ClangClExe       = $null
    ClangRcExe       = $null
    ClangMtExe       = $null
    WindowsFxcExe    = Resolve-WindowsSdkTool -ToolName "fxc.exe"
    BuildDirName     = "build"
    ConfigurePreset  = "windows-default"
    BuildPreset      = "windows-default"
    AutoEnabledNinja = $false
    AutoEnabledClang = $false
  }
}

function Resolve-NinjaToolchainState {
  param(
    [System.Collections.IDictionary]$ToolchainState,
    [pscustomobject]$Options
  )

  $detectedNinja = Resolve-Ninja
  if ($ToolchainState.Ninja) {
    if (-not $detectedNinja) {
      Fail-Build "Ninja requested but ninja.exe was not found. Install Ninja or make sure it is on PATH."
    }
    $ToolchainState.NinjaExe = $detectedNinja
    return
  }

  if ($detectedNinja) {
    $ToolchainState.Ninja = $true
    $ToolchainState.NinjaExe = $detectedNinja
    $ToolchainState.AutoEnabledNinja = $true
  }
}

function Resolve-ClangToolchainState {
  param(
    [System.Collections.IDictionary]$ToolchainState,
    [pscustomobject]$Options
  )

  if (-not $Options.ClangClExplicitlySet -and -not $ToolchainState.ClangCl) {
    $autoClangClExe = Resolve-ClangCl
    if ($autoClangClExe) {
      $ToolchainState.ClangCl = $true
      $ToolchainState.ClangClExe = $autoClangClExe
      $ToolchainState.AutoEnabledClang = $true
    }
  }

  if (-not $ToolchainState.ClangCl) {
    return
  }

  if (-not $ToolchainState.ClangClExe) {
    $ToolchainState.ClangClExe = Resolve-ClangCl
  }
  if (-not $ToolchainState.ClangClExe -and $Options.InstallDeps) {
    Install-ClangClDependency
    $ToolchainState.ClangClExe = Resolve-ClangCl
  }
  if (-not $ToolchainState.ClangClExe) {
    Fail-Build @"
clang-cl requested but clang-cl.exe was not found.

Install LLVM manually:
  winget install --id LLVM.LLVM --exact

Or let the build script do it:
  .\build.ps1 -Static -ClangCl -InstallDeps
"@
  }

  $ToolchainState.ClangRcExe = Resolve-WindowsSdkTool -ToolName "rc.exe"
  if (-not $ToolchainState.ClangRcExe) {
    Fail-Build "clang-cl requested but rc.exe was not found in the Windows SDK."
  }

  $ToolchainState.ClangMtExe = Resolve-WindowsSdkTool -ToolName "mt.exe"
}

function Resolve-BuildToolchainLayoutName {
  param(
    [System.Collections.IDictionary]$ToolchainState,
    [pscustomobject]$Options
  )

  $parts = @()
  if ($ToolchainState.Ninja) {
    $parts += "ninja"
  }
  if ($ToolchainState.ClangCl) {
    $parts += "clangcl"
  }
  if ([bool]$Options.Static) {
    $parts += "static"
  }

  if ($parts.Count -eq 0) {
    return "default"
  }

  return ($parts -join "-")
}

function Resolve-BuildToolchainLayout {
  param(
    [System.Collections.IDictionary]$ToolchainState,
    [pscustomobject]$Options
  )

  $layoutName = Resolve-BuildToolchainLayoutName -ToolchainState $ToolchainState -Options $Options

  if ($ToolchainState.Ninja) {
    $ToolchainState.Generator = "Ninja"
  }

  $ToolchainState.ConfigurePreset = "windows-$layoutName"
  $ToolchainState.BuildPreset = "windows-$layoutName"
  $ToolchainState.BuildDirName = switch ($layoutName) {
    "default" { "build" }
    "static" { "build-static" }
    "ninja" { "build-ninja" }
    "ninja-static" { "build-ninja-static" }
    "clangcl" { "build-clangcl" }
    "clangcl-static" { "build-clangcl-static" }
    "ninja-clangcl" { "build-ninja-clangcl" }
    "ninja-clangcl-static" { "build-ninja-clangcl-static" }
    default { Fail-Build "Unsupported build layout '$layoutName'." }
  }
}

function Resolve-BuildToolchain {
  param([pscustomobject]$Options)

  $toolchainState = New-BuildToolchainState -Options $Options
  Resolve-NinjaToolchainState -ToolchainState $toolchainState -Options $Options
  Resolve-ClangToolchainState -ToolchainState $toolchainState -Options $Options
  Resolve-BuildToolchainLayout -ToolchainState $toolchainState -Options $Options
  return [pscustomobject]$toolchainState
}
