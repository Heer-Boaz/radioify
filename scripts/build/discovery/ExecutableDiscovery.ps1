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
  return Resolve-ExecutablePath `
    -CommandName "clang-cl" `
    -WherePattern "clang-cl.exe" `
    -VisualStudioRelativePaths @(
      "VC\Tools\Llvm\bin\clang-cl.exe",
      "VC\Tools\Llvm\x64\bin\clang-cl.exe",
      "Common7\IDE\VC\VCTools\Llvm\bin\clang-cl.exe",
      "Common7\IDE\VC\VCTools\Llvm\x64\bin\clang-cl.exe"
    ) `
    -DefaultCandidates @(
      "C:\Program Files\LLVM\bin\clang-cl.exe",
      "C:\Program Files (x86)\LLVM\bin\clang-cl.exe",
      $(if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA "Programs\LLVM\bin\clang-cl.exe" })
    )
}

function Resolve-Winget {
  return Resolve-ExecutablePath `
    -CommandName "winget" `
    -WherePattern "winget.exe" `
    -DefaultCandidates @(
      $(if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA "Microsoft\WindowsApps\winget.exe" })
    )
}
