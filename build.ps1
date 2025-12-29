param(
  [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
  [string]$Config = "Release",
  [switch]$Clean
)

$ErrorActionPreference = "Stop"

function Resolve-CMake {
  $cmd = Get-Command cmake -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }

  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (Test-Path $vswhere) {
    $installPath = & $vswhere -latest -products * -property installationPath
    if ($installPath) {
      $candidate = Join-Path $installPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
      if (Test-Path $candidate) { return $candidate }
    }
  }

  $vsRoot = "C:\Program Files\Microsoft Visual Studio\2022"
  $editions = @("Community", "Professional", "Enterprise", "BuildTools")
  foreach ($edition in $editions) {
    $candidate = Join-Path $vsRoot "$edition\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $candidate) { return $candidate }
  }

  return $null
}

function Resolve-VSDevCmd {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (Test-Path $vswhere) {
    $installPath = & $vswhere -latest -products * -property installationPath
    if ($installPath) {
      $candidate = Join-Path $installPath "Common7\Tools\VsDevCmd.bat"
      if (Test-Path $candidate) { return $candidate }
    }
  }

  $vsRoot = "C:\Program Files\Microsoft Visual Studio\2022"
  $editions = @("Community", "Professional", "Enterprise", "BuildTools")
  foreach ($edition in $editions) {
    $candidate = Join-Path $vsRoot "$edition\Common7\Tools\VsDevCmd.bat"
    if (Test-Path $candidate) { return $candidate }
  }

  return $null
}

$cmake = Resolve-CMake
if (-not $cmake) {
  Write-Error "CMake not found. Install 'C++ CMake tools for Windows' or add cmake.exe to PATH."
  exit 1
}

Write-Host "Using CMake: $cmake"

$root = $PSScriptRoot
$buildDir = Join-Path $root "build"

if ($Clean -and (Test-Path $buildDir)) {
  Remove-Item -Recurse -Force $buildDir
}

$cmakeArgs = @("-S", $root, "-B", $buildDir, "-DCMAKE_BUILD_TYPE=$Config")
$buildArgs = @("--build", $buildDir, "--config", $Config)

$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if ($cl) {
  & $cmake @cmakeArgs
  & $cmake @buildArgs
  exit $LASTEXITCODE
}

$vsDevCmd = Resolve-VSDevCmd
if (-not $vsDevCmd) {
  Write-Error "MSVC build tools not found. Install Visual Studio with 'Desktop development with C++' and Windows SDK."
  exit 1
}

Write-Host "Using VS Dev Cmd: $vsDevCmd"

$cmakeCmd = "`"$cmake`" -S `"$root`" -B `"$buildDir`" -DCMAKE_BUILD_TYPE=$Config"
$buildCmd = "`"$cmake`" --build `"$buildDir`" --config $Config"
$cmdLine = "call `"$vsDevCmd`" -arch=x64 -host_arch=x64 && $cmakeCmd && $buildCmd"

& cmd.exe /c $cmdLine
if ($LASTEXITCODE -ne 0) {
  Write-Host ""
  Write-Host "Build failed."
  exit $LASTEXITCODE
}
