param(
  [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
  [string]$Config = "Release",
  [switch]$Clean,
  [switch]$InstallDeps
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

function Copy-FfmpegRuntime {
  param(
    [string]$TripletDir,
    [string]$Config,
    [string]$DistDir
  )

  if (-not $TripletDir) { return }

  $binDir = Join-Path $TripletDir "bin"
  if ($Config -ieq "Debug") {
    $debugBin = Join-Path $TripletDir "debug\\bin"
    if (Test-Path $debugBin) {
      $binDir = $debugBin
    }
  }

  if (-not (Test-Path $binDir)) {
    Write-Warning "FFmpeg runtime bin directory not found: $binDir"
    return
  }

  if (-not (Test-Path $DistDir)) {
    New-Item -ItemType Directory -Force -Path $DistDir | Out-Null
  }

  $dlls = Get-ChildItem -Path $binDir -Filter *.dll -File -ErrorAction SilentlyContinue
  foreach ($dll in $dlls) {
    Copy-Item -Force -Path $dll.FullName -Destination $DistDir
  }
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

$vcpkgRoot = $env:VCPKG_ROOT
if (-not $vcpkgRoot) {
  $vcpkgCmd = Get-Command vcpkg -ErrorAction SilentlyContinue
  if ($vcpkgCmd) {
    $vcpkgRoot = Split-Path -Parent $vcpkgCmd.Source
  }
}
if ($vcpkgRoot) {
  $toolchain = Join-Path $vcpkgRoot "scripts/buildsystems/vcpkg.cmake"
  if (Test-Path $toolchain) {
    $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
  }
}

if ($InstallDeps -and $vcpkgRoot) {
  $vcpkgExe = Join-Path $vcpkgRoot "vcpkg.exe"
  if (-not (Test-Path $vcpkgExe)) {
    $vcpkgExe = "vcpkg"
  }
  $manifest = Join-Path $root "vcpkg.json"
  if (-not (Test-Path $manifest)) {
    Write-Error "vcpkg.json not found. This project requires manifest mode."
    exit 1
  }
  $manifestData = Get-Content $manifest -Raw | ConvertFrom-Json
  if (-not $manifestData.'builtin-baseline') {
    Push-Location $root
    & $vcpkgExe x-update-baseline --add-initial-baseline
    $baselineExit = $LASTEXITCODE
    Pop-Location
    if ($baselineExit -ne 0) {
      Write-Error "Failed to add vcpkg builtin-baseline. Run 'vcpkg x-update-baseline --add-initial-baseline' from the project root."
      exit $baselineExit
    }
  }
  & $vcpkgExe install
  if ($LASTEXITCODE -ne 0) {
    Write-Error "vcpkg install failed."
    exit $LASTEXITCODE
  }
}

if ($vcpkgRoot) {
  $installedRoot = $env:VCPKG_INSTALLED_DIR
  if (-not $installedRoot) {
    $manifestInstalled = Join-Path $root "vcpkg_installed"
    if (Test-Path $manifestInstalled) {
      $installedRoot = $manifestInstalled
    } else {
      $installedRoot = Join-Path $vcpkgRoot "installed"
    }
  }
  if (-not $env:VCPKG_INSTALLED_DIR) {
    $env:VCPKG_INSTALLED_DIR = $installedRoot
  }
  $cmakeArgs += "-DVCPKG_INSTALLED_DIR=$installedRoot"

  $tripletCandidates = @()
  if ($env:VCPKG_TARGET_TRIPLET) { $tripletCandidates += $env:VCPKG_TARGET_TRIPLET }
  if ($env:VCPKG_DEFAULT_TRIPLET) { $tripletCandidates += $env:VCPKG_DEFAULT_TRIPLET }
  $tripletCandidates = $tripletCandidates | Select-Object -Unique

  $foundFfmpeg = $false
  $ffmpegTripletDir = $null
  foreach ($triplet in $tripletCandidates) {
    $candidateDir = Join-Path $installedRoot $triplet
    $ffmpegHeader = Join-Path $candidateDir "include\libavformat\avformat.h"
    if (Test-Path $ffmpegHeader) {
      $foundFfmpeg = $true
      $ffmpegTripletDir = $candidateDir
      break
    }
  }

  if (-not $foundFfmpeg -and (Test-Path $installedRoot)) {
    $tripletDirs = Get-ChildItem -Path $installedRoot -Directory -ErrorAction SilentlyContinue
    foreach ($dir in $tripletDirs) {
      $ffmpegHeader = Join-Path $dir.FullName "include\libavformat\avformat.h"
      if (Test-Path $ffmpegHeader) {
        $foundFfmpeg = $true
        $ffmpegTripletDir = $dir.FullName
        break
      }
    }
  }

  if (-not $foundFfmpeg) {
    Write-Error "FFmpeg not found. Run .\build.ps1 -InstallDeps to install it via vcpkg."
    exit 1
  }
} else {
  Write-Error "vcpkg not found. Install it via the Visual Studio Installer (C++ vcpkg component), then run this script from the 'Developer PowerShell for VS 2022'."
  exit 1
}

$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if ($cl) {
  & $cmake @cmakeArgs
  & $cmake @buildArgs
  $buildExit = $LASTEXITCODE
  if ($buildExit -eq 0) {
    Copy-FfmpegRuntime -TripletDir $ffmpegTripletDir -Config $Config -DistDir (Join-Path $root "dist")
  }
  exit $buildExit
}

$vsDevCmd = Resolve-VSDevCmd
if (-not $vsDevCmd) {
  Write-Error "MSVC build tools not found. Install Visual Studio with 'Desktop development with C++' and Windows SDK."
  exit 1
}

Write-Host "Using VS Dev Cmd: $vsDevCmd"

$cmakeCmd = "`"$cmake`" -S `"$root`" -B `"$buildDir`" -DCMAKE_BUILD_TYPE=$Config"
$vcpkgArg = ""
if ($vcpkgRoot) {
  $toolchain = Join-Path $vcpkgRoot "scripts/buildsystems/vcpkg.cmake"
  if (Test-Path $toolchain) {
    $vcpkgArg = " -DCMAKE_TOOLCHAIN_FILE=`"$toolchain`""
  }
}
$installedArg = ""
if ($installedRoot) {
  $installedArg = " -DVCPKG_INSTALLED_DIR=`"$installedRoot`""
}
$cmakeCmd = $cmakeCmd + $vcpkgArg + $installedArg
$buildCmd = "`"$cmake`" --build `"$buildDir`" --config $Config"
$cmdLine = "call `"$vsDevCmd`" -arch=x64 -host_arch=x64 && $cmakeCmd && $buildCmd"

& cmd.exe /c $cmdLine
if ($LASTEXITCODE -ne 0) {
  Write-Host ""
  Write-Host "Build failed."
  exit $LASTEXITCODE
}

Copy-FfmpegRuntime -TripletDir $ffmpegTripletDir -Config $Config -DistDir (Join-Path $root "dist")
