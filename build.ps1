param(
  [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
  [string]$Config = "Release",
  [switch]$Clean,
  [switch]$Rebuild,
  [switch]$Static,
  [switch]$Ninja,
  [switch]$ClangCl,
  [switch]$VerboseBuild,
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

function Enable-AnsiConsole {
  if (-not $IsWindows) {
    return
  }

  if (-not ("Radioify.ConsoleMode" -as [type])) {
    Add-Type -Namespace Radioify -Name ConsoleMode -MemberDefinition @"
using System;
using System.Runtime.InteropServices;

public static class ConsoleMode {
  [DllImport("kernel32.dll", SetLastError = true)]
  public static extern IntPtr GetStdHandle(int nStdHandle);

  [DllImport("kernel32.dll", SetLastError = true)]
  [return: MarshalAs(UnmanagedType.Bool)]
  public static extern bool GetConsoleMode(IntPtr hConsoleHandle, out uint lpMode);

  [DllImport("kernel32.dll", SetLastError = true)]
  [return: MarshalAs(UnmanagedType.Bool)]
  public static extern bool SetConsoleMode(IntPtr hConsoleHandle, uint dwMode);
}
"@
  }

  $enableVirtualTerminalProcessing = 0x0004
  foreach ($stdHandle in @(-11, -12)) {
    try {
      $handle = [Radioify.ConsoleMode]::GetStdHandle($stdHandle)
      if ($handle -eq [IntPtr]::Zero -or $handle -eq [IntPtr]::new(-1)) {
        continue
      }

      $mode = 0
      if (-not [Radioify.ConsoleMode]::GetConsoleMode($handle, [ref]$mode)) {
        continue
      }

      $targetMode = $mode -bor $enableVirtualTerminalProcessing
      if ($targetMode -ne $mode) {
        [void][Radioify.ConsoleMode]::SetConsoleMode($handle, $targetMode)
      }
    }
    catch {
      continue
    }
  }
}

Enable-AnsiConsole

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

  return ($roots | Select-Object -Unique)
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

  $defaultCandidates = @(
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\vcpkg",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\vcpkg",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\vcpkg",
    "C:\Program Files\Microsoft Visual Studio\2022\Preview\VC\vcpkg",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\vcpkg",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\vcpkg",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\vcpkg",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\vcpkg"
  )

  foreach ($candidate in $defaultCandidates) {
    $candidateExe = Join-Path $candidate "vcpkg.exe"
    if (Test-Path $candidateExe) {
      $candidates += $candidate
    }
  }

  return ($candidates | Select-Object -Unique)
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

function Resolve-CMake {
  $cmd = Get-Command cmake -ErrorAction SilentlyContinue
  if ($cmd -and $cmd.Source -and (Test-Path $cmd.Source)) {
    return $cmd.Source
  }

  foreach ($match in (Invoke-WhereLookup "cmake.exe")) {
    if ($match -and (Test-Path $match)) {
      return $match
    }
  }

  foreach ($installRoot in (Get-VisualStudioInstallRoots)) {
    $candidate = Join-Path $installRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $candidate) {
      return $candidate
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

function Resolve-Ninja {
  $cmd = Get-Command ninja -ErrorAction SilentlyContinue
  if ($cmd -and $cmd.Source -and (Test-Path $cmd.Source)) {
    return $cmd.Source
  }

  foreach ($match in (Invoke-WhereLookup "ninja.exe")) {
    if ($match -and (Test-Path $match)) {
      return $match
    }
  }

  foreach ($installRoot in (Get-VisualStudioInstallRoots)) {
    $candidate = Join-Path $installRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
    if (Test-Path $candidate) {
      return $candidate
    }
  }

  $defaultCandidates = @(
    "C:\Program Files\Ninja\ninja.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Preview\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
  )
  foreach ($candidate in $defaultCandidates) {
    if (Test-Path $candidate) {
      return $candidate
    }
  }

  return $null
}

function Resolve-ClangCl {
  $cmd = Get-Command clang-cl -ErrorAction SilentlyContinue
  if ($cmd -and $cmd.Source -and (Test-Path $cmd.Source)) {
    return $cmd.Source
  }

  foreach ($match in (Invoke-WhereLookup "clang-cl.exe")) {
    if ($match -and (Test-Path $match)) {
      return $match
    }
  }

  foreach ($installRoot in (Get-VisualStudioInstallRoots)) {
    $vsCandidates = @(
      (Join-Path $installRoot "VC\Tools\Llvm\bin\clang-cl.exe"),
      (Join-Path $installRoot "VC\Tools\Llvm\x64\bin\clang-cl.exe"),
      (Join-Path $installRoot "Common7\IDE\VC\VCTools\Llvm\bin\clang-cl.exe"),
      (Join-Path $installRoot "Common7\IDE\VC\VCTools\Llvm\x64\bin\clang-cl.exe")
    )
    foreach ($candidate in $vsCandidates) {
      if (Test-Path $candidate) {
        return $candidate
      }
    }
  }

  $defaultCandidates = @(
    "C:\Program Files\LLVM\bin\clang-cl.exe",
    "C:\Program Files (x86)\LLVM\bin\clang-cl.exe"
  )
  if ($env:LOCALAPPDATA) {
    $defaultCandidates += (Join-Path $env:LOCALAPPDATA "Programs\LLVM\bin\clang-cl.exe")
  }

  foreach ($candidate in $defaultCandidates) {
    if ($candidate -and (Test-Path $candidate)) {
      return $candidate
    }
  }

  return $null
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

function Resolve-WindowsSdkBinary {
  param(
    [Parameter(Mandatory = $true)]
    [string]$BinaryName,
    [string]$Arch = $(Resolve-TargetArch)
  )

  $archVariants = Get-WindowsSdkArchVariants -Arch $Arch
  foreach ($binRoot in (Get-WindowsSdkBinRoots)) {
    foreach ($archVariant in $archVariants) {
      $candidate = Join-Path $binRoot "$archVariant\$BinaryName"
      if (Test-Path $candidate) {
        return $candidate
      }
    }

    $rootCandidate = Join-Path $binRoot $BinaryName
    if (Test-Path $rootCandidate) {
      return $rootCandidate
    }
  }

  return $null
}

function Resolve-Winget {
  $cmd = Get-Command winget -ErrorAction SilentlyContinue
  if ($cmd -and $cmd.Source -and (Test-Path $cmd.Source)) {
    return $cmd.Source
  }

  foreach ($match in (Invoke-WhereLookup "winget.exe")) {
    if ($match -and (Test-Path $match)) {
      return $match
    }
  }

  if ($env:LOCALAPPDATA) {
    $windowsApps = Join-Path $env:LOCALAPPDATA "Microsoft\WindowsApps\winget.exe"
    if (Test-Path $windowsApps) {
      return $windowsApps
    }
  }

  return $null
}

function Resolve-WindowsSdkTool {
  param(
    [Parameter(Mandatory = $true)]
    [string]$ToolName,
    [string]$Arch = "x64"
  )

  $candidates = @()

  if ($env:WindowsSdkDir -and $env:WindowsSDKVersion) {
    $candidates += (Join-Path $env:WindowsSdkDir "bin\$($env:WindowsSDKVersion)\$Arch\$ToolName")
  }

  $kitRoots = @(
    "C:\Program Files (x86)\Windows Kits\10\bin",
    "C:\Program Files\Windows Kits\10\bin"
  )

  foreach ($kitRoot in $kitRoots) {
    if (-not (Test-Path $kitRoot)) {
      continue
    }

    $versionDirs = Get-ChildItem -Path $kitRoot -Directory -ErrorAction SilentlyContinue |
      Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
      Sort-Object { [version]$_.Name } -Descending

    foreach ($dir in $versionDirs) {
      $candidates += (Join-Path $dir.FullName (Join-Path $Arch $ToolName))
    }
  }

  foreach ($candidate in ($candidates | Select-Object -Unique)) {
    if ($candidate -and (Test-Path $candidate)) {
      return $candidate
    }
  }

  return $null
}

function Install-ClangClDependency {
  $wingetExe = Resolve-Winget
  if (-not $wingetExe) {
    Write-Error @"
clang-cl requested with -InstallDeps, but winget.exe was not found.

Install App Installer / winget, or install LLVM manually:
  winget install --id LLVM.LLVM --exact
"@
    exit 1
  }

  $wingetArgs = @(
    "install",
    "--id", "LLVM.LLVM",
    "--exact",
    "--source", "winget",
    "--accept-package-agreements",
    "--accept-source-agreements",
    "--disable-interactivity"
  )

  Write-Host "Installing LLVM via winget: LLVM.LLVM"
  $installExit = Invoke-NativeProcess -FilePath $wingetExe -ArgumentList $wingetArgs
  if ($installExit -ne 0) {
    Write-Error "winget install LLVM.LLVM failed with exit code $installExit."
    exit $installExit
  }
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

  foreach ($candidate in (Get-VisualStudioVcpkgRoots)) {
    if ($candidate -and (Test-Path $candidate)) {
      return $candidate
    }
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

  $visualStudioVcpkgRoots = @(Get-VisualStudioVcpkgRoots)
  $defaultHint = "<Visual Studio>\\VC\\vcpkg"
  if ($visualStudioVcpkgRoots.Count -gt 0) {
    $defaultHint = $visualStudioVcpkgRoots[0]
  }
  $manifestInstalled = Join-Path $RepoRoot "vcpkg_installed"

  Write-Error @"
vcpkg kon niet automatisch worden gevonden, maar is wel nodig voor deze build.

Waarom dit vaak gebeurt (vooral in WSL2):
- vcpkg.exe staat niet op PATH in deze PowerShell sessie.
- VCPKG_ROOT is niet gezet voor deze sessie/gebruiker.

Zo los je het op:
1) Laat Visual Studio de root dynamisch bepalen:
   `$vswhere = Join-Path `${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
   `$env:VCPKG_ROOT = Join-Path (& `$vswhere -latest -products * -property installationPath) "VC\vcpkg"

2) Eenmalig expliciet meegeven:
   .\build.ps1 -Static -InstallDeps -VcpkgRoot "$defaultHint"

3) Of environment variabele zetten (PowerShell):
   `$env:VCPKG_ROOT="$defaultHint"

4) Persistente user-waarde:
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

function Prepend-PathDirectory {
  param([string]$PathToAdd)

  if (-not $PathToAdd) { return }
  $resolved = [System.IO.Path]::GetFullPath($PathToAdd)
  if (-not (Test-Path $resolved)) { return }

  $parts = @()
  $existingPath = $env:PATH
  if (-not $existingPath) {
    $existingPath = ""
  }

  foreach ($part in ($existingPath -split ';')) {
    if (-not $part) { continue }
    if ([string]::Equals($part, $resolved, [System.StringComparison]::OrdinalIgnoreCase)) {
      continue
    }
    $parts += $part
  }

  $env:PATH = ($resolved + $(if ($parts.Count -gt 0) { ";" + ($parts -join ';') } else { "" }))
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

function Format-WindowsCommandLineArgument {
  param([AllowEmptyString()][string]$Argument)

  if ($null -eq $Argument) { return '""' }
  if ($Argument -eq "") { return '""' }
  if ($Argument -notmatch '[\s"]') { return $Argument }

  $builder = New-Object System.Text.StringBuilder
  [void]$builder.Append('"')

  $backslashCount = 0
  foreach ($char in $Argument.ToCharArray()) {
    if ($char -eq '\') {
      $backslashCount++
      continue
    }

    if ($char -eq '"') {
      [void]$builder.Append(('\' * ($backslashCount * 2 + 1)))
      [void]$builder.Append('"')
      $backslashCount = 0
      continue
    }

    if ($backslashCount -gt 0) {
      [void]$builder.Append(('\' * $backslashCount))
      $backslashCount = 0
    }

    [void]$builder.Append($char)
  }

  if ($backslashCount -gt 0) {
    [void]$builder.Append(('\' * ($backslashCount * 2)))
  }

  [void]$builder.Append('"')
  return $builder.ToString()
}

function Join-WindowsCommandLineArguments {
  param([string[]]$ArgumentList = @())

  if (-not $ArgumentList -or $ArgumentList.Count -eq 0) {
    return ""
  }

  $formatted = @()
  foreach ($arg in $ArgumentList) {
    $formatted += (Format-WindowsCommandLineArgument -Argument $arg)
  }
  return ($formatted -join ' ')
}

function Invoke-NativeProcess {
  param(
    [Parameter(Mandatory = $true)]
    [string]$FilePath,
    [string[]]$ArgumentList = @(),
    [string]$WorkingDirectory = $null
  )

  $formattedArgumentList = Join-WindowsCommandLineArguments -ArgumentList $ArgumentList
  $startArgs = @{
    FilePath    = $FilePath
    NoNewWindow = $true
    Wait        = $true
    PassThru    = $true
  }
  if ($formattedArgumentList) {
    $startArgs.ArgumentList = $formattedArgumentList
  }
  if ($WorkingDirectory) {
    $startArgs.WorkingDirectory = $WorkingDirectory
  }

  $process = Start-Process @startArgs
  if ($null -eq $process) {
    return 0
  }
  return [int]$process.ExitCode
}

function Convert-ToCMakePath {
  param([string]$Path)
  if (-not $Path) { return $null }
  return ([System.IO.Path]::GetFullPath($Path) -replace "\\", "/")
}

function Convert-ToCMakePathList {
  param([string]$PathList)
  if (-not $PathList) { return $null }

  $parts = @()
  foreach ($part in ($PathList -split ';')) {
    if ($part) {
      $parts += (Convert-ToCMakePath $part)
    }
  }
  return ($parts -join ';')
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

function Normalize-RemainingSwitchArguments {
  param([string[]]$RemainingArguments)

  if (-not $RemainingArguments -or $RemainingArguments.Count -eq 0) {
    return
  }

  $knownSwitches = @(
    "Clean",
    "Rebuild",
    "Static",
    "Ninja",
    "ClangCl",
    "VerboseBuild",
    "InstallDeps",
    "MelodyAnalysis",
    "TimingLog",
    "StagingUpload",
    "VideoErrorLog",
    "FfmpegErrorLog"
  )

  $unexpected = @()
  foreach ($arg in $RemainingArguments) {
    if (-not $arg) { continue }

    $normalized = $null
    foreach ($switchName in $knownSwitches) {
      if ($arg -imatch "^-{1,2}$([regex]::Escape($switchName))[\\/]+$") {
        $normalized = $switchName
        break
      }
    }

    if ($normalized) {
      Write-Warning "Treating '$arg' as '-$normalized'. Remove the trailing slash/backslash."
      Set-Variable -Scope Script -Name $normalized -Value $true
      continue
    }

    $unexpected += $arg
  }

  if ($unexpected.Count -gt 0) {
    Write-Error ("Unknown arguments: {0}" -f ($unexpected -join ", "))
    exit 1
  }
}

function Resolve-BuildTriplet {
  param([bool]$PreferStatic = $false)

  if ($env:VCPKG_TARGET_TRIPLET) {
    return $env:VCPKG_TARGET_TRIPLET
  }
  if ($env:VCPKG_DEFAULT_TRIPLET) {
    return $env:VCPKG_DEFAULT_TRIPLET
  }
  if ($PreferStatic) {
    return Resolve-StaticTriplet
  }
  return "$(Resolve-TargetArch)-windows"
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

function Find-RadioifyExecutable {
  param(
    [string]$Root,
    [string]$BuildDir,
    [string]$Config
  )

  $candidates = @()
  if ($Root) {
    $candidates += (Join-Path $Root "dist\radioify.exe")
  }
  if ($BuildDir) {
    $candidates += (Join-Path $BuildDir "radioify.exe")
    if ($Config) {
      $candidates += (Join-Path (Join-Path $BuildDir $Config) "radioify.exe")
    }
  }

  foreach ($candidate in ($candidates | Select-Object -Unique)) {
    if ($candidate -and (Test-Path $candidate)) {
      return $candidate
    }
  }

  if ($BuildDir -and (Test-Path $BuildDir)) {
    $found = Get-ChildItem -Path $BuildDir -Filter "radioify.exe" -File -Recurse -ErrorAction SilentlyContinue |
      Sort-Object FullName |
      Select-Object -First 1
    if ($found) {
      return $found.FullName
    }
  }

  return $null
}

$cmake = Resolve-CMake
if (-not $cmake) {
  Write-Error "CMake not found. Install CMake and ensure cmake.exe is on PATH."
  exit 1
}

Write-Host "Using CMake: $cmake"

$root = $PSScriptRoot
$distDir = Join-Path $root "dist"

# Detect/resolve Ninja. If the user explicitly requested -Ninja, require it; otherwise
# auto-enable Ninja when ninja.exe is available on the system so builds default to Ninja.
$ninjaExe = $null
if ($Ninja) {
  $ninjaExe = Resolve-Ninja
  if (-not $ninjaExe) {
    Write-Error "Ninja requested but ninja.exe was not found. Install Ninja or make sure it is on PATH."
    exit 1
  }
}
else {
  $detectedNinja = Resolve-Ninja
  if ($detectedNinja) {
    $Ninja = $true
    $ninjaExe = $detectedNinja
    Write-Host "Auto-enabled Ninja: $ninjaExe"
  }
}

$buildDirName = if ($Ninja) { "build-ninja" } else { "build" }
$clangClExplicitlySet = $PSBoundParameters.ContainsKey("ClangCl")
$clangClExe = $null
$clangRcExe = $null
$clangMtExe = $null
$windowsFxcExe = Resolve-WindowsSdkTool -ToolName "fxc.exe"
if (-not $clangClExplicitlySet) {
  $autoClangClExe = Resolve-ClangCl
  if ($autoClangClExe) {
    $ClangCl = $true
    $clangClExe = $autoClangClExe
    Write-Host "Auto-enabled clang-cl: $clangClExe"
  }
}
if ($ClangCl) {
  if (-not $clangClExe) {
    $clangClExe = Resolve-ClangCl
  }
  if (-not $clangClExe -and $InstallDeps) {
    Install-ClangClDependency
    $clangClExe = Resolve-ClangCl
  }
  if (-not $clangClExe) {
    Write-Error @"
clang-cl requested but clang-cl.exe was not found.

Install LLVM manually:
  winget install --id LLVM.LLVM --exact

Or let the build script do it:
  .\build.ps1 -Static -ClangCl -InstallDeps
"@
    exit 1
  }

  $clangRcExe = Resolve-WindowsSdkTool -ToolName "rc.exe"
  if (-not $clangRcExe) {
    Write-Error "clang-cl requested but rc.exe was not found in the Windows SDK."
    exit 1
  }

  $clangMtExe = Resolve-WindowsSdkTool -ToolName "mt.exe"
  if ($clangMtExe) {
    Prepend-PathDirectory (Split-Path -Parent $clangMtExe)
  }
  Prepend-PathDirectory (Split-Path -Parent $clangRcExe)
  Prepend-PathDirectory (Split-Path -Parent $clangClExe)
}

if ($ClangCl) {
  $buildDirName = if ($Ninja) { "build-ninja-clangcl" } else { "build-clangcl" }
}
$buildDir = Join-Path $root $buildDirName

$cmakeGenerator = if ($Ninja) { "Ninja" } else { $null }
if ($Ninja) {
  Write-Host "Using generator: $cmakeGenerator"
  Write-Host "Using Ninja: $ninjaExe"
  if (-not $env:NINJA_STATUS) {
    $env:NINJA_STATUS = "[%p %f/%t | %w elapsed | ETA %W] "
  }
  Write-Host "Using Ninja status: $env:NINJA_STATUS"
}
if ($ClangCl) {
  Write-Host "Using compiler: $clangClExe"
  Write-Host "Using Windows SDK rc: $clangRcExe"
  if ($clangMtExe) {
    Write-Host "Using Windows SDK mt: $clangMtExe"
  }
  if ($windowsFxcExe) {
    Write-Host "Using shader compiler: $windowsFxcExe"
  }
}

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

Normalize-RemainingSwitchArguments -RemainingArguments $args

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

$effectiveTargetTriplet = Resolve-BuildTriplet -PreferStatic:$Static
$effectiveDefaultTriplet = $env:VCPKG_DEFAULT_TRIPLET
if (-not $effectiveDefaultTriplet) {
  $effectiveDefaultTriplet = $effectiveTargetTriplet
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
  if ($effectiveTargetTriplet) {
    $vcpkgInstallArgs += @("--triplet", $effectiveTargetTriplet)
  }
  if ($MelodyAnalysis) {
    $vcpkgInstallArgs += "--x-feature=melody-analysis"
  }

  Write-Host "Using vcpkg: $vcpkgExe"
  $installExit = Invoke-NativeProcess -FilePath $vcpkgExe -ArgumentList $vcpkgInstallArgs -WorkingDirectory $root

  if ($installExit -ne 0) {
    Write-Host ""
    Write-Host "If the error mentions a missing/invalid Visual Studio instance, install the C++ build tools (Desktop development with C++) and try again." -ForegroundColor Yellow
    Write-Error "vcpkg install failed with exit code $installExit."
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

$rootForCMake = Convert-ToCMakePath $root
$buildDirForCMake = Convert-ToCMakePath $buildDir
$installedRootForCMake = Convert-ToCMakePath $installedRoot
$cmakeArgs = @("-S", $rootForCMake, "-B", $buildDirForCMake, "-Wno-deprecated")
if ($cmakeGenerator) {
  $cmakeArgs += @("-G", $cmakeGenerator)
  if ($ninjaExe) {
    $cmakeArgs += "-DCMAKE_MAKE_PROGRAM=$(Convert-ToCMakePath $ninjaExe)"
  }
  # For single-config Ninja generator, forward the desired config explicitly.
  if ($cmakeGenerator -eq "Ninja") {
    $cmakeArgs += "-DCMAKE_BUILD_TYPE=$Config"
  }
}
else {
  $cmakeArgs += "-DCMAKE_BUILD_TYPE=$Config"
}
if ($ClangCl) {
  $clangClForCMake = Convert-ToCMakePath $clangClExe
  $cmakeArgs += "-DCMAKE_C_COMPILER=$clangClForCMake"
  $cmakeArgs += "-DCMAKE_CXX_COMPILER=$clangClForCMake"
  $cmakeArgs += "-DCMAKE_C_FLAGS_INIT=/clang:-fcolor-diagnostics /clang:-fansi-escape-codes /clang:-fcommon"
  $cmakeArgs += "-DCMAKE_CXX_FLAGS_INIT=/clang:-fcolor-diagnostics /clang:-fansi-escape-codes"
  $cmakeArgs += "-DCMAKE_RC_COMPILER=$(Convert-ToCMakePath $clangRcExe)"
  if ($clangMtExe) {
    $cmakeArgs += "-DCMAKE_MT=$(Convert-ToCMakePath $clangMtExe)"
  }
}
if ($windowsFxcExe) {
  $cmakeArgs += "-DRADIOIFY_FXC_EXE=$(Convert-ToCMakePath $windowsFxcExe)"
}
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
if (Is-StaticTriplet $effectiveTargetTriplet) {
  # Static triplets do not need vcpkg's post-build DLL deployment step.
  # Disabling it avoids the applocal.ps1/dumpbin warning noise.
  $cmakeArgs += "-DVCPKG_APPLOCAL_DEPS=OFF"
}
if ($vcpkgRoot) {
  $toolchain = Join-Path $vcpkgRoot "scripts/buildsystems/vcpkg.cmake"
  if (Test-Path $toolchain) {
    $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$(Convert-ToCMakePath $toolchain)"
  }
}
if ($installedRoot) {
  $cmakeArgs += "-DVCPKG_INSTALLED_DIR=$installedRootForCMake"
}
if ($effectiveTargetTriplet) {
  $cmakeArgs += "-DVCPKG_TARGET_TRIPLET=$effectiveTargetTriplet"
}
if ($env:VCPKG_OVERLAY_PORTS) {
  $cmakeArgs += "-DVCPKG_OVERLAY_PORTS=$(Convert-ToCMakePathList $env:VCPKG_OVERLAY_PORTS)"
}

if ($MelodyAnalysis -and (Is-StaticTriplet $effectiveTargetTriplet)) {
  Assert-OnnxStaticRegistrationDisabled -InstalledRoot $installedRoot -Triplet $effectiveTargetTriplet
}

$foundFfmpeg = $false
$ffmpegTripletDir = $null
if ($installedRoot -and (Test-Path $installedRoot)) {
  $tripletCandidates = @()
  if ($effectiveTargetTriplet) { $tripletCandidates += $effectiveTargetTriplet }
  if ($effectiveDefaultTriplet) { $tripletCandidates += $effectiveDefaultTriplet }
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

  if (-not $foundFfmpeg -and -not $effectiveTargetTriplet -and -not $effectiveDefaultTriplet) {
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
  if ($effectiveTargetTriplet) {
    Write-Error "FFmpeg not found for triplet '$effectiveTargetTriplet'. Run .\build.ps1 -InstallDeps$(if ($Static) { ' -Static' }) or set FFMPEG_DIR/FFMPEG_ROOT."
  }
  else {
    Write-Error "FFmpeg not found. Run .\build.ps1 -InstallDeps (vcpkg) or set FFMPEG_DIR/FFMPEG_ROOT."
  }
  exit 1
}
if (-not ($env:FFMPEG_DIR -or $env:FFMPEG_ROOT) -and $foundFfmpeg -and $ffmpegTripletDir) {
  $env:FFMPEG_DIR = Convert-ToCMakePath $ffmpegTripletDir
}
elseif ($env:FFMPEG_DIR) {
  $env:FFMPEG_DIR = Convert-ToCMakePath $env:FFMPEG_DIR
}
elseif ($env:FFMPEG_ROOT) {
  $env:FFMPEG_ROOT = Convert-ToCMakePath $env:FFMPEG_ROOT
}

$cachePath = Join-Path $buildDir "CMakeCache.txt"
$cachedManifestMode = Get-CMakeCacheValue -CachePath $cachePath -Variable "VCPKG_MANIFEST_MODE"
$initialManifestMode = Get-CMakeCacheValue -CachePath $cachePath -Variable "Z_VCPKG_CHECK_MANIFEST_MODE"
$cachedToolchainFile = Get-CMakeCacheValue -CachePath $cachePath -Variable "CMAKE_TOOLCHAIN_FILE"
$effectiveCachedManifestMode = $null
if ($initialManifestMode) {
  $effectiveCachedManifestMode = $initialManifestMode
}
elseif ($cachedManifestMode) {
  $effectiveCachedManifestMode = $cachedManifestMode
}

$desiredToolchainForCache = $null
if ($vcpkgRoot) {
  $toolchain = Join-Path $vcpkgRoot "scripts/buildsystems/vcpkg.cmake"
  if (Test-Path $toolchain) {
    $desiredToolchainForCache = Convert-ToCMakePath $toolchain
  }
}

if ($effectiveCachedManifestMode -and ($effectiveCachedManifestMode.ToUpperInvariant() -ne $desiredManifestMode)) {
  Write-Host "Detected VCPKG_MANIFEST_MODE mismatch in build cache: $effectiveCachedManifestMode -> $desiredManifestMode"
  Write-Host "Recreating build directory to avoid unsupported cache transition."
  if (Test-Path $buildDir) {
    Remove-PathWithRetry -Path $buildDir
  }
}
elseif ($desiredToolchainForCache -and $cachedToolchainFile -and ($cachedToolchainFile -ne $desiredToolchainForCache)) {
  Write-Host "Detected CMAKE_TOOLCHAIN_FILE mismatch in build cache:"
  Write-Host "  cached : $cachedToolchainFile"
  Write-Host "  desired: $desiredToolchainForCache"
  Write-Host "Recreating build directory to clear stale broken CMake cache."
  if (Test-Path $buildDir) {
    Remove-PathWithRetry -Path $buildDir
  }
}

$configureExit = Invoke-NativeProcess -FilePath $cmake -ArgumentList $cmakeArgs -WorkingDirectory $root
if ($configureExit -ne 0) {
  exit $configureExit
}

$buildArgs = @("--build", $buildDirForCMake, "--target", "radioify")
if (-not $Ninja) {
  $buildArgs += @("--config", $Config)
}
if ($VerboseBuild) {
  $buildArgs += "--verbose"
}

$buildExit = Invoke-NativeProcess -FilePath $cmake -ArgumentList $buildArgs -WorkingDirectory $root
if ($buildExit -eq 0) {
  $staticTriplet = $tripletStaticRequested -or (Is-StaticTriplet $effectiveTargetTriplet) -or (Is-StaticTriplet $effectiveDefaultTriplet)
  if (-not $Static -and -not $staticTriplet) {
    Copy-FfmpegRuntime -TripletDir $ffmpegTripletDir -Config $Config -DistDir (Join-Path $root "dist")
  }

  $distPath = Join-Path $root "dist"
  $expectedExe = Join-Path $distPath "radioify.exe"
  $builtExe = Find-RadioifyExecutable -Root $root -BuildDir $buildDir -Config $Config
  if ($builtExe -and ($builtExe -ne $expectedExe)) {
    if (-not (Test-Path $distPath)) {
      New-Item -ItemType Directory -Force -Path $distPath | Out-Null
    }
    Copy-Item -Force -Path $builtExe -Destination $expectedExe

    $pdbSource = [System.IO.Path]::ChangeExtension($builtExe, ".pdb")
    if (Test-Path $pdbSource) {
      $pdbDest = Join-Path $distPath ([System.IO.Path]::GetFileName($pdbSource))
      Copy-Item -Force -Path $pdbSource -Destination $pdbDest
    }
  }

  if (-not (Test-Path $expectedExe)) {
    Write-Error "Build completed without producing $expectedExe. Check the build output above."
    exit 1
  }

  if (Test-Path $distPath) {
    Write-Host "Build artifacts written to: $distPath"
    Get-ChildItem -Path $distPath -Recurse -File | ForEach-Object {
      Write-Host " - $($_.FullName)"
    }
    Write-Host "Run with: .\dist\radioify.exe <file-or-folder>"
  }
}

exit $buildExit
