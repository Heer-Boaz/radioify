function Resolve-TargetArch {
  $arch = $env:VSCMD_ARG_TGT_ARCH
  if (-not $arch) { $arch = $env:Platform }
  if (-not $arch) { $arch = $env:PROCESSOR_ARCHITECTURE }
  if (-not $arch) { return "x64" }

  $archLower = $arch.ToLower()
  if ($archLower -eq "win32") { return "x86" }
  if ($archLower -eq "amd64") { return "x64" }
  if ($archLower -eq "arm64") { return "arm64" }
  if ($archLower -eq "x86") { return "x86" }
  if ($archLower -eq "x64") { return "x64" }
  return "x64"
}

function Get-WindowsSdkArchVariants {
  param([string]$Arch = $(Resolve-TargetArch))

  $normalized = "x64"
  if ($Arch) {
    $archLower = $Arch.ToLowerInvariant()
    if ($archLower -eq "x86" -or $archLower -eq "win32") {
      $normalized = "x86"
    }
    elseif ($archLower -eq "arm64") {
      $normalized = "arm64"
    }
  }

  return @($normalized)
}

function Get-WindowsSdkBinRoots {
  $roots = @()

  if ($env:WindowsSdkDir -and (Test-Path $env:WindowsSdkDir)) {
    $sdkDir = $env:WindowsSdkDir.TrimEnd('\', '/')
    if ($env:WindowsSDKVersion) {
      $sdkVersion = $env:WindowsSDKVersion.Trim('\', '/')
      $candidate = Join-Path $sdkDir "bin\$sdkVersion"
      if (Test-Path $candidate) {
        $roots += $candidate
      }
    }

    $sdkBinRoot = Join-Path $sdkDir "bin"
    if (Test-Path $sdkBinRoot) {
      $versionDirs = Get-ChildItem -Path $sdkBinRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending
      foreach ($dir in $versionDirs) {
        $roots += $dir.FullName
      }
    }
  }

  $defaultSdkBinRoot = "C:\Program Files (x86)\Windows Kits\10\bin"
  if (Test-Path $defaultSdkBinRoot) {
    $versionDirs = Get-ChildItem -Path $defaultSdkBinRoot -Directory -ErrorAction SilentlyContinue |
      Sort-Object Name -Descending
    foreach ($dir in $versionDirs) {
      $roots += $dir.FullName
    }
  }

  return ($roots | Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique)
}

function Resolve-WindowsSdkTool {
  param(
    [Parameter(Mandatory = $true)]
    [string]$ToolName,
    [string]$Arch = "x64"
  )

  $archVariants = Get-WindowsSdkArchVariants -Arch $Arch
  foreach ($binRoot in (Get-WindowsSdkBinRoots)) {
    foreach ($archVariant in $archVariants) {
      $candidate = Join-Path $binRoot "$archVariant\$ToolName"
      if (Test-Path $candidate) {
        return $candidate
      }
    }

    $rootCandidate = Join-Path $binRoot $ToolName
    if (Test-Path $rootCandidate) {
      return $rootCandidate
    }
  }

  return $null
}
