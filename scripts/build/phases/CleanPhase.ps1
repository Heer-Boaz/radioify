function Invoke-CleanStep {
  param([pscustomobject]$Context)

  if ($Context.Options.Rebuild) {
    $Context.Options.Clean = $true
  }

  if (-not $Context.Options.Clean) {
    return $false
  }

  foreach ($path in @($Context.Paths.BuildDir, $Context.Paths.DistDir)) {
    if ($path -and (Test-Path $path)) {
      Remove-PathWithRetry -Path $path
    }
  }

  if (-not $Context.Options.Rebuild) {
    Write-Host "Cleaned build directory: $($Context.Paths.BuildDir)"
    Write-Host "Cleaned dist directory: $($Context.Paths.DistDir)"
    return $true
  }

  return $false
}
