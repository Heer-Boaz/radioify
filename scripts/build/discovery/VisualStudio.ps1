function Get-VswhereCandidates {
  $vswhereCandidates = @()
  if ($env:ProgramFiles -and (Test-Path $env:ProgramFiles)) {
    $vswhereCandidates += (Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe")
  }
  if (${env:ProgramFiles(x86)} -and (Test-Path ${env:ProgramFiles(x86)})) {
    $vswhereCandidates += (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe")
  }

  return ($vswhereCandidates |
    Where-Object { $_ -and (Test-Path $_) } |
    Select-Object -Unique)
}

function Get-VisualStudioKnownInstallRoots {
  $roots = @()

  $programFilesRoots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}) |
    Where-Object { $_ -and (Test-Path $_) } |
    Select-Object -Unique

  foreach ($programFilesRoot in $programFilesRoots) {
    $visualStudioRoot = Join-Path $programFilesRoot "Microsoft Visual Studio"
    if (-not (Test-Path $visualStudioRoot)) {
      continue
    }

    $versionDirs = Get-ChildItem -Path $visualStudioRoot -Directory -ErrorAction SilentlyContinue |
      Where-Object { $_.Name -ne "Installer" }
    foreach ($versionDir in $versionDirs) {
      $editionDirs = Get-ChildItem -Path $versionDir.FullName -Directory -ErrorAction SilentlyContinue
      foreach ($editionDir in $editionDirs) {
        $hasVisualStudioMarkers = (Test-Path (Join-Path $editionDir.FullName "Common7")) -or
          (Test-Path (Join-Path $editionDir.FullName "VC"))
        if ($hasVisualStudioMarkers) {
          $roots += $editionDir.FullName
        }
      }
    }
  }

  return ($roots | Select-Object -Unique)
}

function Get-VisualStudioInstallRoots {
  $roots = @()

  foreach ($vswhere in (Get-VswhereCandidates)) {
    $installRoots = & $vswhere -all -products * -property installationPath 2>$null
    foreach ($installRoot in $installRoots) {
      if ($installRoot -and (Test-Path $installRoot)) {
        $roots += $installRoot
      }
    }
  }

  $roots += Get-VisualStudioKnownInstallRoots
  return ($roots | Select-Object -Unique)
}

function Get-VisualStudioExecutableCandidates {
  param([string[]]$RelativePaths)

  $candidates = @()
  foreach ($installRoot in (Get-VisualStudioInstallRoots)) {
    foreach ($relativePath in $RelativePaths) {
      $candidate = Join-Path $installRoot $relativePath
      if ($candidate -and (Test-Path $candidate)) {
        $candidates += $candidate
      }
    }
  }

  return ($candidates | Select-Object -Unique)
}

function Get-VisualStudioVcpkgRoots {
  $candidates = @()

  foreach ($installRoot in (Get-VisualStudioInstallRoots)) {
    $candidate = Join-Path $installRoot "VC\vcpkg"
    $candidateExe = Join-Path $candidate "vcpkg.exe"
    if (Test-Path $candidateExe) {
      $candidates += $candidate
    }
  }

  return ($candidates | Select-Object -Unique)
}
