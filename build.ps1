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
  return $null
}

function Resolve-VcpkgRoot {
  $vcpkgRoot = $env:VCPKG_ROOT
  if ($vcpkgRoot -and (Test-Path $vcpkgRoot)) { return $vcpkgRoot }

  $vcpkgCmd = Get-Command vcpkg -ErrorAction SilentlyContinue
  if ($vcpkgCmd) {
    $candidate = Split-Path -Parent $vcpkgCmd.Source
    if ($candidate -and (Test-Path $candidate)) { return $candidate }
  }

  return $null
}

function Resolve-VcpkgExe {
  param([string]$VcpkgRoot)

  if ($VcpkgRoot) {
    $exe = Join-Path $VcpkgRoot "vcpkg.exe"
    if (Test-Path $exe) { return $exe }
  }

  $cmd = Get-Command vcpkg -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }

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
  Write-Error "CMake not found. Install CMake and ensure cmake.exe is on PATH."
  exit 1
}

Write-Host "Using CMake: $cmake"

$root = $PSScriptRoot
$buildDir = Join-Path $root "build"

if ($Clean -and (Test-Path $buildDir)) {
  Remove-Item -Recurse -Force $buildDir
}

$vcpkgRoot = Resolve-VcpkgRoot
$vcpkgExe = Resolve-VcpkgExe -VcpkgRoot $vcpkgRoot

if ($InstallDeps) {
  if (-not $vcpkgExe) {
    Write-Error "vcpkg not found. Set VCPKG_ROOT or ensure vcpkg.exe is on PATH."
    exit 1
  }

  $manifest = Join-Path $root "vcpkg.json"
  if (-not (Test-Path $manifest)) {
    Write-Error "vcpkg.json not found. This project requires manifest mode."
    exit 1
  }

  Push-Location $root
  & $vcpkgExe install
  $installExit = $LASTEXITCODE
  Pop-Location

  if ($installExit -ne 0) {
    Write-Host ""
    Write-Host "If the error mentions a missing/invalid Visual Studio instance, install the C++ build tools (Desktop development with C++) and try again." -ForegroundColor Yellow
    Write-Error "vcpkg install failed."
    exit $installExit
  }
}

$installedRoot = $env:VCPKG_INSTALLED_DIR
if (-not $installedRoot) {
  $manifestInstalled = Join-Path $root "vcpkg_installed"
  if (Test-Path $manifestInstalled) {
    $installedRoot = $manifestInstalled
  }
  elseif ($vcpkgRoot) {
    $installedRoot = Join-Path $vcpkgRoot "installed"
  }
}

$cmakeArgs = @("-S", $root, "-B", $buildDir, "-DCMAKE_BUILD_TYPE=$Config")
if ($vcpkgRoot) {
  $toolchain = Join-Path $vcpkgRoot "scripts/buildsystems/vcpkg.cmake"
  if (Test-Path $toolchain) {
    $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
  }
}
if ($installedRoot) {
  $cmakeArgs += "-DVCPKG_INSTALLED_DIR=$installedRoot"
}

$buildArgs = @("--build", $buildDir, "--config", $Config)

$foundFfmpeg = $false
$ffmpegTripletDir = $null
if ($installedRoot -and (Test-Path $installedRoot)) {
  $tripletCandidates = @()
  if ($env:VCPKG_TARGET_TRIPLET) { $tripletCandidates += $env:VCPKG_TARGET_TRIPLET }
  if ($env:VCPKG_DEFAULT_TRIPLET) { $tripletCandidates += $env:VCPKG_DEFAULT_TRIPLET }
  $tripletCandidates = $tripletCandidates | Select-Object -Unique

  foreach ($triplet in $tripletCandidates) {
    $candidateDir = Join-Path $installedRoot $triplet
    $ffmpegHeader = Join-Path $candidateDir "include\libavformat\avformat.h"
    if (Test-Path $ffmpegHeader) {
      $foundFfmpeg = $true
      $ffmpegTripletDir = $candidateDir
      break
    }
  }

  if (-not $foundFfmpeg) {
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
}

if (-not $foundFfmpeg -and -not ($env:FFMPEG_DIR -or $env:FFMPEG_ROOT)) {
  Write-Error "FFmpeg not found. Run .\build.ps1 -InstallDeps (vcpkg) or set FFMPEG_DIR/FFMPEG_ROOT."
  exit 1
}

& $cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) {
  exit $LASTEXITCODE
}

& $cmake @buildArgs
$buildExit = $LASTEXITCODE
if ($buildExit -eq 0) {
  Copy-FfmpegRuntime -TripletDir $ffmpegTripletDir -Config $Config -DistDir (Join-Path $root "dist")
}

exit $buildExit
