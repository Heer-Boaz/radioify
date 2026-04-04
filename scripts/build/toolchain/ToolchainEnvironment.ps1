function Apply-BuildToolchainEnvironment {
  param([pscustomobject]$Toolchain)

  if ($Toolchain.ClangCl) {
    if ($Toolchain.ClangMtExe) {
      Prepend-PathDirectory (Split-Path -Parent $Toolchain.ClangMtExe)
    }
    Prepend-PathDirectory (Split-Path -Parent $Toolchain.ClangRcExe)
    Prepend-PathDirectory (Split-Path -Parent $Toolchain.ClangClExe)
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
