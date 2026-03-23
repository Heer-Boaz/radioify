param(
  [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
  [string]$Config = "Release",
  [switch]$Clean,
  [switch]$Rebuild,
  [switch]$Static,
  [switch]$InstallDeps,
  [Alias("VCPKG_ROOT")]
  [string]$VcpkgRoot,
  [Alias("SongAnalysis")]
  [switch]$MelodyAnalysis,
  [switch]$TimingLog,
  [switch]$StagingUpload,
  [switch]$VideoErrorLog,
  [switch]$FfmpegErrorLog
)

$ErrorActionPreference = "Stop"

function Resolve-CMake {
  $cmd = Get-Command cmake -ErrorAction SilentlyContinue
  if ($cmd -and $cmd.Source -and (Test-Path $cmd.Source)) {
    return $cmd.Source
  }

  $whereExe = Get-Command where.exe -ErrorAction SilentlyContinue
  if ($whereExe) {
    $whereMatches = & $whereExe.Source cmake.exe 2>$null
    foreach ($match in $whereMatches) {
      if ($match -and (Test-Path $match)) {
        return $match
      }
    }
  }

  $vswhereCandidates = @()
  if ($env:ProgramFiles -and (Test-Path $env:ProgramFiles)) {
    $vswhereCandidates += (Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe")
  }
  if (${env:ProgramFiles(x86)} -and (Test-Path ${env:ProgramFiles(x86)})) {
    $vswhereCandidates += (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe")
  }

  foreach ($vswhere in ($vswhereCandidates | Select-Object -Unique)) {
    if (-not (Test-Path $vswhere)) { continue }
    $installRoots = & $vswhere -all -products * -property installationPath 2>$null
    foreach ($installRoot in $installRoots) {
      if (-not $installRoot) { continue }
      $candidate = Join-Path $installRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
      if (Test-Path $candidate) {
        return $candidate
      }
    }
  }

  $defaultCandidates = @(
    "C:\Program Files\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Preview\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  )
  foreach ($candidate in $defaultCandidates) {
    if (Test-Path $candidate) {
      return $candidate
    }
  }

  return $null
}

function Resolve-VcpkgRoot {
  param([string]$PreferredRoot)

  $vcpkgRoot = $PreferredRoot
  if ($vcpkgRoot -and (Test-Path $vcpkgRoot)) { return $vcpkgRoot }

  $vcpkgRoot = $env:VCPKG_ROOT
  if ($vcpkgRoot -and (Test-Path $vcpkgRoot)) { return $vcpkgRoot }

  $vcpkgCmd = Get-Command vcpkg -ErrorAction SilentlyContinue
  if ($vcpkgCmd) {
    $candidate = Split-Path -Parent $vcpkgCmd.Source
    if ($candidate -and (Test-Path $candidate)) { return $candidate }
  }

  return $null
}

function Assert-ExplicitVcpkgRootValid {
  param([string]$ProvidedRoot)

  if (-not $ProvidedRoot) { return }
  if (-not (Test-Path $ProvidedRoot)) {
    Write-Error "Explicit -VcpkgRoot path does not exist: $ProvidedRoot"
    exit 1
  }

  $exe = Join-Path $ProvidedRoot "vcpkg.exe"
  if (-not (Test-Path $exe)) {
    Write-Error "Explicit -VcpkgRoot is missing vcpkg.exe: $ProvidedRoot"
    exit 1
  }
}

function Assert-VcpkgRootReadableWhenRequired {
  param(
    [string]$VcpkgRoot,
    [string]$VcpkgExe,
    [string]$RepoRoot,
    [bool]$NeedsVcpkg
  )

  if (-not $NeedsVcpkg) { return }
  if ($VcpkgRoot -and (Test-Path $VcpkgRoot) -and $VcpkgExe) { return }

  $defaultHint = "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\vcpkg"
  $manifestInstalled = Join-Path $RepoRoot "vcpkg_installed"

  Write-Error @"
vcpkg kon niet automatisch worden gevonden, maar is wel nodig voor deze build.

Waarom dit vaak gebeurt (vooral in WSL2):
- vcpkg.exe staat niet op PATH in deze PowerShell sessie.
- VCPKG_ROOT is niet gezet voor deze sessie/gebruiker.

Zo los je het op:
1) Eenmalig meegeven:
   .\build.ps1 -Static -InstallDeps -VcpkgRoot "$defaultHint"

2) Of environment variabele zetten (PowerShell):
   `$env:VCPKG_ROOT="$defaultHint"

3) Persistente user-waarde:
   [Environment]::SetEnvironmentVariable("VCPKG_ROOT", "$defaultHint", "User")

Extra context:
- Script root: $RepoRoot
- Verwachte manifest install map: $manifestInstalled
"@
  exit 1
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

function Remove-PathWithRetry {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,
    [int]$MaxRetries = 12,
    [int]$DelayMilliseconds = 500
  )

  if (-not (Test-Path $Path)) { return }

  for ($attempt = 1; $attempt -le $MaxRetries; $attempt++) {
    try {
      Remove-Item -Recurse -Force $Path -ErrorAction Stop
      return
    }
    catch {
      if ($attempt -eq 4 -and (Test-Path $Path)) {
        Stop-BuildProcessesUsingPath -TargetPath $Path
      }

      if ($attempt -ge $MaxRetries) {
        Write-Error "Could not delete path '$Path' after $MaxRetries retries: $($_.Exception.Message)"
        exit 1
      }

      Write-Warning "Delete attempt $attempt failed for '$Path'. Retrying in $DelayMilliseconds ms..."
      Start-Sleep -Milliseconds $DelayMilliseconds
    }
  }
}

function Stop-BuildProcessesUsingPath {
  param([string]$TargetPath)
  if (-not $TargetPath) { return }

  $pathMarker = "*$TargetPath*"
  $candidateNames = @(
    "cmake.exe",
    "MSBuild.exe",
    "ninja.exe",
    "cl.exe",
    "link.exe",
    "devenv.exe"
  )

  $processes = Get-CimInstance Win32_Process | Where-Object {
    $_.CommandLine -and $_.CommandLine -like $pathMarker -and ($candidateNames -contains $_.Name)
  }
  if (-not $processes) {
    return
  }

  Write-Warning "Stopping processes that still lock build path '$TargetPath':"
  foreach ($process in $processes) {
    Write-Warning " - $($process.Name) ($($process.ProcessId))"
    Stop-Process -Id $process.ProcessId -Force -ErrorAction SilentlyContinue
  }
  Start-Sleep -Milliseconds 500
}

function Assert-OnnxOverlayPortComplete {
  param([string]$OverlayPortsRoot)

  if (-not $OverlayPortsRoot) { return }
  $onnxPortDir = Join-Path $OverlayPortsRoot "onnx"
  if (-not (Test-Path $onnxPortDir)) { return }

  $requiredFiles = @(
    "portfile.cmake",
    "vcpkg.json",
    "fix-cmakelists.patch",
    "fix-pr-7390.patch"
  )

  $missing = @()
  foreach ($name in $requiredFiles) {
    $path = Join-Path $onnxPortDir $name
    if (-not (Test-Path $path)) {
      $missing += $path
    }
  }

  if ($missing.Count -gt 0) {
    Write-Error @"
ONNX overlay port is incomplete. Missing required files:
$($missing -join "`n")

Restore these files in: $onnxPortDir
"@
    exit 1
  }
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

function Get-CMakeCacheValue {
  param(
    [string]$CachePath,
    [string]$Variable
  )

  if (-not $CachePath -or -not (Test-Path $CachePath)) { return $null }
  if (-not $Variable) { return $null }

  $pattern = "^{0}:(?:BOOL|STRING|PATH|FILEPATH|INTERNAL)=([^\r\n]*)$" -f [regex]::Escape($Variable)
  $match = Select-String -Path $CachePath -Pattern $pattern -CaseSensitive |
    Select-Object -First 1
  if (-not $match) { return $null }

  return $match.Matches[0].Groups[1].Value
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
  Remove-PathWithRetry -Path $buildDir
}
if ($Clean -and (Test-Path $distDir)) {
  Remove-PathWithRetry -Path $distDir
}
if ($Clean -and -not $Rebuild) {
  Write-Host "Cleaned build directory: $buildDir"
  Write-Host "Cleaned dist directory: $distDir"
  exit 0
}

Assert-ExplicitVcpkgRootValid -ProvidedRoot $VcpkgRoot
$vcpkgRoot = Resolve-VcpkgRoot -PreferredRoot $VcpkgRoot
$vcpkgExe = Resolve-VcpkgExe -VcpkgRoot $vcpkgRoot

if ($MelodyAnalysis) {
  # ONNX Runtime static builds require ONNX static registration to be disabled
  # to avoid runtime duplicate schema registration errors.
  Add-VcpkgCMakeConfigureOption "-DONNX_DISABLE_STATIC_REGISTRATION=ON"

  $overlayPortsDir = Join-Path $root "vcpkg-overlays\ports"
  if (Test-Path $overlayPortsDir) {
    Assert-OnnxOverlayPortComplete $overlayPortsDir
    Add-VcpkgOverlayPortPath $overlayPortsDir
    Write-Host "Using vcpkg overlay ports: $overlayPortsDir"
  }
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
  Assert-VcpkgRootReadableWhenRequired -VcpkgRoot $vcpkgRoot -VcpkgExe $vcpkgExe -RepoRoot $root -NeedsVcpkg $true

  $manifest = Join-Path $root "vcpkg.json"
  if (-not (Test-Path $manifest)) {
    Write-Error "vcpkg.json not found. This project requires manifest mode."
    exit 1
  }

  $vcpkgInstallArgs = @(
    "install",
    "--clean-buildtrees-after-build",
    "--clean-packages-after-build"
  )
  if ($MelodyAnalysis) {
    $vcpkgInstallArgs += "--x-feature=melody-analysis"
  }

  Push-Location $root
  & $vcpkgExe @vcpkgInstallArgs
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

$manifestInstalledAvailable = [bool]($installedRoot -and (Test-Path $installedRoot))
$needsVcpkgForBuild = -not ([bool]($env:FFMPEG_DIR -or $env:FFMPEG_ROOT)) -and -not $manifestInstalledAvailable
Assert-VcpkgRootReadableWhenRequired -VcpkgRoot $vcpkgRoot -VcpkgExe $vcpkgExe -RepoRoot $root -NeedsVcpkg $needsVcpkgForBuild

$cmakeArgs = @("-S", $root, "-B", $buildDir, "-DCMAKE_BUILD_TYPE=$Config")
$cmakeArgs += "-DRADIOIFY_ENABLE_TIMING_LOG=$([bool]$TimingLog)"
$cmakeArgs += "-DRADIOIFY_ENABLE_STAGING_UPLOAD=$([bool]$StagingUpload)"
$cmakeArgs += "-DRADIOIFY_ENABLE_VIDEO_ERROR_LOG=$([bool]$VideoErrorLog)"
$cmakeArgs += "-DRADIOIFY_ENABLE_FFMPEG_ERROR_LOG=$([bool]$FfmpegErrorLog)"
$cmakeArgs += "-DRADIOIFY_ENABLE_MELODY_ANALYSIS=$([bool]$MelodyAnalysis)"
$cmakeArgs += "-DRADIOIFY_ENABLE_NEURAL_PITCH=$([bool]$MelodyAnalysis)"
# Never let CMake/toolchain auto-run `vcpkg install`.
# Dependencies are installed explicitly only via `-InstallDeps`.
$desiredManifestMode = "OFF"
$cmakeArgs += "-DVCPKG_MANIFEST_MODE=$desiredManifestMode"
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
if ($env:FFMPEG_DIR) {
  $cmakeArgs += "-DFFMPEG_DIR=$($env:FFMPEG_DIR)"
}
if ($env:FFMPEG_ROOT) {
  $cmakeArgs += "-DFFMPEG_ROOT=$($env:FFMPEG_ROOT)"
}
if ($env:VCPKG_OVERLAY_PORTS) {
  $cmakeArgs += "-DVCPKG_OVERLAY_PORTS=$env:VCPKG_OVERLAY_PORTS"
}

if ($MelodyAnalysis -and (Is-StaticTriplet $env:VCPKG_TARGET_TRIPLET)) {
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
if (-not ($env:FFMPEG_DIR -or $env:FFMPEG_ROOT) -and $foundFfmpeg -and $ffmpegTripletDir) {
  $env:FFMPEG_DIR = $ffmpegTripletDir
  $cmakeArgs += "-DFFMPEG_DIR=$($env:FFMPEG_DIR)"
}

$cachePath = Join-Path $buildDir "CMakeCache.txt"
$cachedManifestMode = Get-CMakeCacheValue -CachePath $cachePath -Variable "VCPKG_MANIFEST_MODE"
$initialManifestMode = Get-CMakeCacheValue -CachePath $cachePath -Variable "Z_VCPKG_CHECK_MANIFEST_MODE"
$effectiveCachedManifestMode = $null
if ($initialManifestMode) {
  $effectiveCachedManifestMode = $initialManifestMode
}
elseif ($cachedManifestMode) {
  $effectiveCachedManifestMode = $cachedManifestMode
}

if ($effectiveCachedManifestMode -and ($effectiveCachedManifestMode.ToUpperInvariant() -ne $desiredManifestMode)) {
  Write-Host "Detected VCPKG_MANIFEST_MODE mismatch in build cache: $effectiveCachedManifestMode -> $desiredManifestMode"
  Write-Host "Recreating build directory to avoid unsupported cache transition."
  if (Test-Path $buildDir) {
    Remove-PathWithRetry -Path $buildDir
  }
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
