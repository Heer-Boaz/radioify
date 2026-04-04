function Add-VcpkgCMakeConfigureOption {
  param([string]$Option)

  if (-not $Option) { return }
  Add-UniqueEnvironmentListValue -Name "VCPKG_CMAKE_CONFIGURE_OPTIONS" -Value $Option
}

function Add-VcpkgOverlayPortPath {
  param([string]$PathToAdd)

  if (-not $PathToAdd) { return }
  $resolved = [System.IO.Path]::GetFullPath($PathToAdd)
  if (-not (Test-Path $resolved)) { return }

  Add-UniqueEnvironmentListValue -Name "VCPKG_OVERLAY_PORTS" -Value $resolved -CaseInsensitive
}
