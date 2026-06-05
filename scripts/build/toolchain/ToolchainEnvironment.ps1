function Apply-BuildToolchainEnvironment {
  param([pscustomobject]$Toolchain)

  if ($Toolchain.Ninja -and $Toolchain.NinjaExe) {
    Prepend-PathDirectory (Split-Path -Parent $Toolchain.NinjaExe)
  }

  if ($Toolchain.ClangCl) {
    if ($Toolchain.ClangMtExe) {
      Prepend-PathDirectory (Split-Path -Parent $Toolchain.ClangMtExe)
    }
    Prepend-PathDirectory (Split-Path -Parent $Toolchain.ClangRcExe)
    Prepend-PathDirectory (Split-Path -Parent $Toolchain.ClangClExe)
  }

  # If using MSVC (not clang-cl), ensure MSVC and Windows SDK tools are on PATH so
  # CMake can invoke rc.exe/mt.exe/link.exe correctly when not running from a
  # Developer Prompt.
  if (-not $Toolchain.ClangCl) {
    try {
      if (Get-Command Resolve-Cl -ErrorAction SilentlyContinue) {
        $msvcCl = Resolve-Cl
      }
      else {
        $msvcCl = Resolve-ExecutablePath -CommandName "cl.exe" -WherePattern "cl.exe" -VisualStudioRelativePaths @(
          "VC\Tools\MSVC\bin\Hostx64\x64\cl.exe",
          "VC\Tools\MSVC\bin\Hostx64\x86\cl.exe",
          "VC\bin\cl.exe"
        )
      }
      if ($msvcCl) {
        Prepend-PathDirectory (Split-Path -Parent $msvcCl)
        $rc = Resolve-WindowsSdkTool -ToolName "rc.exe"
        if ($rc) { Prepend-PathDirectory (Split-Path -Parent $rc) }
        $mt = Resolve-WindowsSdkTool -ToolName "mt.exe"
        if ($mt) { Prepend-PathDirectory (Split-Path -Parent $mt) }
        $link = Resolve-ExecutablePath -CommandName "link.exe" -WherePattern "link.exe" -VisualStudioRelativePaths @(
          "VC\Tools\MSVC\bin\Hostx64\x64\link.exe",
          "VC\Tools\MSVC\bin\Hostx64\x86\link.exe"
        )
        if ($link) { Prepend-PathDirectory (Split-Path -Parent $link) }
      }
    }
    catch {
      # best-effort only
    }
  }

  if ($Toolchain.Ninja -and -not (Get-ProcessEnvironmentVariable -Name "NINJA_STATUS")) {
    Set-ProcessEnvironmentVariable -Name "NINJA_STATUS" -Value "[%p %f/%t | %w elapsed | ETA %W] "
  }
}

function Write-BuildToolchainSummary {
  param([pscustomobject]$Toolchain)

  if ($Toolchain.AutoEnabledNinja) {
    Write-Host "Auto-enabled Ninja: $($Toolchain.NinjaExe)"
  }
  if ($Toolchain.AutoEnabledClang) {
    Write-Host "Auto-enabled clang-cl: $($Toolchain.ClangClExe)"
  }

  if ($Toolchain.Ninja) {
    Write-Host "Using generator: $($Toolchain.Generator)"
    Write-Host "Using Ninja: $($Toolchain.NinjaExe)"
    Write-Host "Using Ninja status: $(Get-ProcessEnvironmentVariable -Name "NINJA_STATUS")"
  }

  Write-Host "Using configure preset: $($Toolchain.ConfigurePreset)"
  Write-Host "Using build preset: $($Toolchain.BuildPreset)"

  if ($Toolchain.ClangCl) {
    Write-Host "Using compiler: $($Toolchain.ClangClExe)"
    Write-Host "Using Windows SDK rc: $($Toolchain.ClangRcExe)"
    if ($Toolchain.ClangMtExe) {
      Write-Host "Using Windows SDK mt: $($Toolchain.ClangMtExe)"
    }
    if ($Toolchain.WindowsFxcExe) {
      Write-Host "Using shader compiler: $($Toolchain.WindowsFxcExe)"
    }
  }
}
