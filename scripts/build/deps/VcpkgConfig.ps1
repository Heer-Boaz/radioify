function Add-VcpkgOverlayPortPath {
  param([string]$PathToAdd)

  if (-not $PathToAdd) { return }
  $resolved = [System.IO.Path]::GetFullPath($PathToAdd)
  if (-not (Test-Path $resolved)) { return }

  Add-UniqueEnvironmentListValue -Name "VCPKG_OVERLAY_PORTS" -Value $resolved -CaseInsensitive
}

function Add-VcpkgOverlayTripletPath {
  param([string]$Path)

  if (-not $Path) { return }
  $resolved = (Resolve-Path $Path).Path
  Add-UniqueEnvironmentListValue -Name "VCPKG_OVERLAY_TRIPLETS" -Value $resolved -CaseInsensitive
}

function Configure-RepositoryVcpkgOverlays {
  param([pscustomobject]$Context)

  if ($Context.Tools.VcpkgRoot) {
    Set-ProcessEnvironmentVariable `
      -Name "RADIOIFY_VCPKG_ROOT" `
      -Value ([System.IO.Path]::GetFullPath($Context.Tools.VcpkgRoot))
  }

  $overlayPortsDir = Join-Path $Context.Paths.Root "vcpkg-overlays\ports"
  if (-not (Test-Path $overlayPortsDir)) { return }

  Add-VcpkgOverlayPortPath $overlayPortsDir
  Write-Host "Using vcpkg overlay ports: $overlayPortsDir"

  $overlayTripletsDir = Join-Path $Context.Paths.Root "vcpkg-overlays\triplets"
  if (Test-Path $overlayTripletsDir) {
    Add-VcpkgOverlayTripletPath $overlayTripletsDir
    Write-Host "Using vcpkg overlay triplets: $overlayTripletsDir"
  }

  $aomClangCl = $Context.Tools.Toolchain.ClangClExe
  if (-not $aomClangCl) { $aomClangCl = Resolve-ClangCl }
  if ($aomClangCl) {
    Set-ProcessEnvironmentVariable -Name "RADIOIFY_AOM_CLANG_CL" -Value $aomClangCl
  }

  $windowsRc = $Context.Tools.Toolchain.ClangRcExe
  if (-not $windowsRc) {
    $windowsRc = Resolve-WindowsSdkTool -ToolName "rc.exe"
  }
  if ($windowsRc) {
    Set-ProcessEnvironmentVariable -Name "RADIOIFY_AOM_RC" -Value $windowsRc
  }
}
