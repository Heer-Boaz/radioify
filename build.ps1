param(
  [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
  [string]$Config = "Release",
  [switch]$Clean,
  [switch]$Rebuild,
  [switch]$Static,
  [switch]$InstallDeps,
  [switch]$TimingLog,
  [switch]$StagingUpload,
  [switch]$VideoErrorLog,
  [switch]$FfmpegErrorLog
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

function Add-VcpkgCMakeConfigureOption {
  param([string]$Option)

  if (-not $Option) { return }
  $existing = $env:VCPKG_CMAKE_CONFIGURE_OPTIONS
  if (-not $existing) {
    $env:VCPKG_CMAKE_CONFIGURE_OPTIONS = $Option
    return
  }

  $parts = @()
  foreach ($part in ($existing -split ';')) {
    if ($part) { $parts += $part }
  }
  if ($parts -contains $Option) { return }
  $parts += $Option
  $env:VCPKG_CMAKE_CONFIGURE_OPTIONS = ($parts -join ';')
}

function Add-VcpkgOverlayPortPath {
  param([string]$PathToAdd)

  if (-not $PathToAdd) { return }
  $resolved = [System.IO.Path]::GetFullPath($PathToAdd)
  if (-not (Test-Path $resolved)) { return }

  $existing = $env:VCPKG_OVERLAY_PORTS
  if (-not $existing) {
    $env:VCPKG_OVERLAY_PORTS = $resolved
    return
  }

  $parts = @()
  foreach ($part in ($existing -split ';')) {
    if ($part) { $parts += $part }
  }
  if ($parts -contains $resolved) { return }
  $parts += $resolved
  $env:VCPKG_OVERLAY_PORTS = ($parts -join ';')
}

function Resolve-StaticTriplet {
  param([string]$Arch)
  if (-not $Arch) { $Arch = Resolve-TargetArch }
  return "$Arch-windows-static"
}

function Is-StaticTriplet {
  param([string]$Triplet)
  if (-not $Triplet) { return $false }
  return $Triplet -match "-static"
}

function Convert-ToStaticTriplet {
  param([string]$Triplet)
  if (-not $Triplet) { return Resolve-StaticTriplet }
  if (Is-StaticTriplet $Triplet) { return $Triplet }
  if ($Triplet -match "^(.*)-windows-md$") {
    return "$($matches[1])-windows-static-md"
  }
  if ($Triplet -match "^(.*)-windows$") {
    return "$($matches[1])-windows-static"
  }
  return Resolve-StaticTriplet
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

function Assert-OnnxStaticRegistrationDisabled {
  param(
    [string]$InstalledRoot,
    [string]$Triplet
  )

  if (-not $InstalledRoot) { return }
  $onnxBuildDir = Join-Path $InstalledRoot "vcpkg\blds\onnx"
  if (-not (Test-Path $onnxBuildDir)) { return }

  $candidates = @()
  if ($Triplet) {
    $candidates += (Join-Path $onnxBuildDir ("config-{0}-out.log" -f $Triplet))
  }
  $candidates += Get-ChildItem -Path $onnxBuildDir -Filter "config-*-out.log" -File -ErrorAction SilentlyContinue |
    Select-Object -ExpandProperty FullName
  $candidates = $candidates | Select-Object -Unique

  foreach ($logPath in $candidates) {
    if (-not (Test-Path $logPath)) { continue }
    $isOff = Select-String -Path $logPath -Pattern "ONNX_DISABLE_STATIC_REGISTRATION\\s*:\\s*OFF" -SimpleMatch:$false -Quiet
    if ($isOff) {
      Write-Error @"
ONNX is currently built with ONNX_DISABLE_STATIC_REGISTRATION=OFF.
That causes Melody mode schema spam/crashes with ONNX Runtime static builds.

Fix:
  1) Delete this folder: $InstalledRoot
  2) Re-run: .\\build.ps1 -Static

This build now ships an ONNX overlay port that forces ONNX_DISABLE_STATIC_REGISTRATION=ON.
"@
      exit 1
    }
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
$distDir = Join-Path $root "dist"

if ($Rebuild) {
  $Clean = $true
}

if ($Clean -and (Test-Path $buildDir)) {
  Remove-Item -Recurse -Force $buildDir
}
if ($Clean -and (Test-Path $distDir)) {
  Remove-Item -Recurse -Force $distDir
}
if ($Clean -and -not $Rebuild) {
  Write-Host "Cleaned build directory: $buildDir"
  Write-Host "Cleaned dist directory: $distDir"
  exit 0
}

$vcpkgRoot = Resolve-VcpkgRoot
$vcpkgExe = Resolve-VcpkgExe -VcpkgRoot $vcpkgRoot

# ONNX Runtime static builds require ONNX static registration to be disabled to
# avoid runtime duplicate schema registration errors.
Add-VcpkgCMakeConfigureOption "-DONNX_DISABLE_STATIC_REGISTRATION=ON"

$overlayPortsDir = Join-Path $root "vcpkg-overlays\ports"
if (Test-Path $overlayPortsDir) {
  Add-VcpkgOverlayPortPath $overlayPortsDir
  Write-Host "Using vcpkg overlay ports: $overlayPortsDir"
}

$tripletStaticRequested = $false
if ($Static) {
  $requestedTriplet = $env:VCPKG_TARGET_TRIPLET
  $staticTriplet = Convert-ToStaticTriplet $requestedTriplet
  if ($requestedTriplet -and ($staticTriplet -ne $requestedTriplet)) {
    Write-Host "Static build: overriding VCPKG_TARGET_TRIPLET '$requestedTriplet' -> '$staticTriplet'"
  }
  $env:VCPKG_TARGET_TRIPLET = $staticTriplet
  $requestedDefaultTriplet = $env:VCPKG_DEFAULT_TRIPLET
  if (-not $requestedDefaultTriplet -or -not (Is-StaticTriplet $requestedDefaultTriplet)) {
    if ($requestedDefaultTriplet -and ($requestedDefaultTriplet -ne $staticTriplet)) {
      Write-Host "Static build: overriding VCPKG_DEFAULT_TRIPLET '$requestedDefaultTriplet' -> '$staticTriplet'"
    }
    $env:VCPKG_DEFAULT_TRIPLET = $staticTriplet
  }
  $tripletStaticRequested = $true
}

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
$cmakeArgs += "-DRADIOIFY_ENABLE_TIMING_LOG=$([bool]$TimingLog)"
$cmakeArgs += "-DRADIOIFY_ENABLE_STAGING_UPLOAD=$([bool]$StagingUpload)"
$cmakeArgs += "-DRADIOIFY_ENABLE_VIDEO_ERROR_LOG=$([bool]$VideoErrorLog)"
$cmakeArgs += "-DRADIOIFY_ENABLE_FFMPEG_ERROR_LOG=$([bool]$FfmpegErrorLog)"
# Never let CMake/toolchain auto-run `vcpkg install`.
# Dependencies are installed explicitly only via `-InstallDeps`.
$cmakeArgs += "-DVCPKG_MANIFEST_MODE=OFF"
$cmakeArgs += "-DVCPKG_MANIFEST_INSTALL=OFF"
if ($vcpkgRoot) {
  $toolchain = Join-Path $vcpkgRoot "scripts/buildsystems/vcpkg.cmake"
  if (Test-Path $toolchain) {
    $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
  }
}
if ($installedRoot) {
  $cmakeArgs += "-DVCPKG_INSTALLED_DIR=$installedRoot"
}
if ($env:VCPKG_TARGET_TRIPLET) {
  $cmakeArgs += "-DVCPKG_TARGET_TRIPLET=$env:VCPKG_TARGET_TRIPLET"
}
if ($env:VCPKG_OVERLAY_PORTS) {
  $cmakeArgs += "-DVCPKG_OVERLAY_PORTS=$env:VCPKG_OVERLAY_PORTS"
}

if (Is-StaticTriplet $env:VCPKG_TARGET_TRIPLET) {
  Assert-OnnxStaticRegistrationDisabled -InstalledRoot $installedRoot -Triplet $env:VCPKG_TARGET_TRIPLET
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
  $staticTriplet = $tripletStaticRequested -or (Is-StaticTriplet $env:VCPKG_TARGET_TRIPLET) -or (Is-StaticTriplet $env:VCPKG_DEFAULT_TRIPLET)
  if (-not $Static -and -not $staticTriplet) {
    Copy-FfmpegRuntime -TripletDir $ffmpegTripletDir -Config $Config -DistDir (Join-Path $root "dist")
  }
}

exit $buildExit
