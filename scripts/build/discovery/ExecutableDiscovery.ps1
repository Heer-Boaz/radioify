function Get-FirstExistingPath {
  param([string[]]$Candidates)

  foreach ($candidate in $Candidates) {
    if ($candidate -and (Test-Path $candidate)) {
      return $candidate
    }
  }

  return $null
}

function Invoke-WhereLookup {
  param([string]$Pattern)

  $whereExe = Get-Command where.exe -ErrorAction SilentlyContinue
  if (-not $whereExe) {
    return @()
  }

  $restoreNativePreference = $false
  $nativePreference = $false
  if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $restoreNativePreference = $true
    $nativePreference = $PSNativeCommandUseErrorActionPreference
    $PSNativeCommandUseErrorActionPreference = $false
  }

  try {
    $matches = & $whereExe.Source $Pattern 2>$null
    if ($null -eq $matches) {
      return @()
    }
    return @($matches)
  }
  catch {
    return @()
  }
  finally {
    if ($restoreNativePreference) {
      $PSNativeCommandUseErrorActionPreference = $nativePreference
    }
  }
}

function Resolve-ExecutablePath {
  param(
    [string]$CommandName,
    [string]$WherePattern,
    [string[]]$VisualStudioRelativePaths = @(),
    [string[]]$DefaultCandidates = @()
  )

  if ($CommandName) {
    $cmd = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source -and (Test-Path $cmd.Source)) {
      return $cmd.Source
    }
  }

  if ($WherePattern) {
    foreach ($match in (Invoke-WhereLookup $WherePattern)) {
      if ($match -and (Test-Path $match)) {
        return $match
      }
    }
  }

  if ($VisualStudioRelativePaths.Count -gt 0) {
    $visualStudioMatch = Get-FirstExistingPath (Get-VisualStudioExecutableCandidates -RelativePaths $VisualStudioRelativePaths)
    if ($visualStudioMatch) {
      return $visualStudioMatch
    }
  }

  return Get-FirstExistingPath $DefaultCandidates
}

function Resolve-CMake {
  return Resolve-ExecutablePath `
    -CommandName "cmake" `
    -WherePattern "cmake.exe" `
    -VisualStudioRelativePaths @("Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe") `
    -DefaultCandidates @(
      "C:\Program Files\CMake\bin\cmake.exe"
    )
}

function Resolve-Ninja {
  return Resolve-ExecutablePath `
    -CommandName "ninja" `
    -WherePattern "ninja.exe" `
    -VisualStudioRelativePaths @("Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe") `
    -DefaultCandidates @(
      "C:\Program Files\Ninja\ninja.exe"
    )
}

function Resolve-ClangCl {
  $visualStudioMatch = Get-FirstExistingPath (Get-VisualStudioExecutableCandidates -RelativePaths @(
    "VC\Tools\Llvm\x64\bin\clang-cl.exe",
    "Common7\IDE\VC\VCTools\Llvm\x64\bin\clang-cl.exe",
    "VC\Tools\Llvm\bin\clang-cl.exe",
    "Common7\IDE\VC\VCTools\Llvm\bin\clang-cl.exe"
  ))
  if ($visualStudioMatch) {
    return $visualStudioMatch
  }

  return Resolve-ExecutablePath `
    -CommandName "clang-cl" `
    -WherePattern "clang-cl.exe" `
    -DefaultCandidates @(
      "C:\Program Files\LLVM\bin\clang-cl.exe",
      "C:\Program Files (x86)\LLVM\bin\clang-cl.exe",
      $(if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA "Programs\LLVM\bin\clang-cl.exe" })
    )
}

function Resolve-Cl {
  <#
  Resolve the full path to MSVC's cl.exe.
  Tries several strategies in order:
   - Get-Command (PATH)
   - where.exe
   - Visual Studio install roots under VC\Tools\MSVC\<version>\bin\Host*\*\cl.exe
   - common legacy VC\bin path
  #>

  # 1) cl.exe on PATH
  $cmd = Get-Command "cl.exe" -ErrorAction SilentlyContinue
  if ($cmd -and $cmd.Source -and (Test-Path $cmd.Source)) {
    return $cmd.Source
  }

  # 2) where.exe lookup
  foreach ($match in (Invoke-WhereLookup "cl.exe")) {
    if ($match -and (Test-Path $match)) { return $match }
  }

  # 3) Search Visual Studio install roots for VC\Tools\MSVC\<version>\bin\Host*\*\cl.exe
  foreach ($installRoot in (Get-VisualStudioInstallRoots)) {
    $msvcRoot = Join-Path $installRoot "VC\Tools\MSVC"
    if (-not (Test-Path $msvcRoot)) { continue }

    $versionDirs = Get-ChildItem -Path $msvcRoot -Directory -ErrorAction SilentlyContinue |
      Sort-Object Name -Descending
    foreach ($ver in $versionDirs) {
      # prefer 64-bit host/target
      $candidates = @(
        (Join-Path $ver.FullName "bin\Hostx64\x64\cl.exe"),
        (Join-Path $ver.FullName "bin\Hostx64\x86\cl.exe"),
        (Join-Path $ver.FullName "bin\Hostx86\x86\cl.exe")
      )
      foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { return $candidate }
      }
    }

    # 4) legacy path
    $legacy = Join-Path $installRoot "VC\bin\cl.exe"
    if (Test-Path $legacy) { return $legacy }
  }

  return $null
}

function Resolve-Winget {
  return Resolve-ExecutablePath `
    -CommandName "winget" `
    -WherePattern "winget.exe" `
    -DefaultCandidates @(
      $(if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA "Microsoft\WindowsApps\winget.exe" })
    )
}
