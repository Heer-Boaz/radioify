function Install-ClangClDependency {
  $wingetExe = Resolve-Winget
  if (-not $wingetExe) {
    Fail-Build @"
clang-cl requested with -InstallDeps, but winget.exe was not found.

Install App Installer / winget, or install LLVM manually:
  winget install --id LLVM.LLVM --exact
"@
  }

  $wingetArgs = @(
    "install",
    "--id", "LLVM.LLVM",
    "--exact",
    "--source", "winget",
    "--accept-package-agreements",
    "--accept-source-agreements",
    "--disable-interactivity"
  )

  Write-Host "Installing LLVM via winget: LLVM.LLVM"
  $installExit = Invoke-NativeProcess -FilePath $wingetExe -ArgumentList $wingetArgs
  if ($installExit -ne 0) {
    Fail-Build "winget install LLVM.LLVM failed with exit code $installExit." -ExitCode $installExit
  }
}
