function Test-NvidiaRtxVideoSdkRoot {
  param([string]$SdkRoot)

  if (-not $SdkRoot -or -not (Test-Path -LiteralPath $SdkRoot)) {
    return $false
  }

  $includeDir = Join-Path $SdkRoot "include"
  $libDir = Join-Path $SdkRoot "lib\x64"
  $header = Join-Path $includeDir "nvsdk_ngx.h"
  $defsHeader = Join-Path $includeDir "nvsdk_ngx_defs.h"
  $staticLib = Join-Path $libDir "nvsdk_ngx_s.lib"
  $dynamicLib = Join-Path $libDir "nvsdk_ngx_d.lib"

  return (Test-Path -LiteralPath $header) -and
         (Test-Path -LiteralPath $defsHeader) -and
         ((Test-Path -LiteralPath $staticLib) -or
          (Test-Path -LiteralPath $dynamicLib))
}

function Find-NvidiaRtxVideoSdkRootInTree {
  param([string]$SearchRoot)

  if (-not $SearchRoot -or -not (Test-Path -LiteralPath $SearchRoot)) {
    return $null
  }

  if (Test-NvidiaRtxVideoSdkRoot -SdkRoot $SearchRoot) {
    return (Get-Item -LiteralPath $SearchRoot).FullName
  }

  $headers = Get-ChildItem -LiteralPath $SearchRoot -Recurse -File `
    -Filter "nvsdk_ngx.h" -ErrorAction SilentlyContinue |
    Sort-Object FullName

  foreach ($header in $headers) {
    $includeDir = Split-Path -Parent $header.FullName
    if ((Split-Path -Leaf $includeDir) -ine "include") {
      continue
    }
    $candidateRoot = Split-Path -Parent $includeDir
    if (Test-NvidiaRtxVideoSdkRoot -SdkRoot $candidateRoot) {
      return (Get-Item -LiteralPath $candidateRoot).FullName
    }
  }

  return $null
}

function Get-NvidiaRtxVideoSdkCandidateRoots {
  param([string]$RepoRoot)

  $candidates = @()
  foreach ($name in @(
      "RADIOIFY_NVIDIA_RTX_VIDEO_SDK_ROOT",
      "NVIDIA_RTX_VIDEO_SDK_ROOT",
      "RTX_VIDEO_SDK_ROOT",
      "NGX_SDK_ROOT",
      "NGX_SDK_DIR")) {
    $value = Get-ProcessEnvironmentVariable -Name $name
    if ($value) {
      $candidates += $value
    }
  }

  if ($RepoRoot) {
    $candidates += (Join-Path $RepoRoot "third_party\nvidia\rtx-video-sdk")
  }

  $candidates += @(
    "C:\ProgramData\NVIDIA Corporation\NGX SDK",
    "C:\Program Files\NVIDIA Corporation\RTX Video SDK",
    "C:\Program Files\NVIDIA Corporation\NGX SDK",
    "C:\Program Files (x86)\NVIDIA Corporation\NGX SDK"
  )

  return $candidates | Where-Object { $_ } | Select-Object -Unique
}

function Resolve-NvidiaRtxVideoSdkRoot {
  param([string]$RepoRoot)

  foreach ($candidate in (Get-NvidiaRtxVideoSdkCandidateRoots -RepoRoot $RepoRoot)) {
    $root = Find-NvidiaRtxVideoSdkRootInTree -SearchRoot $candidate
    if ($root) {
      return $root
    }
  }

  return $null
}

function Get-NvidiaRtxVideoSdkArchivePath {
  foreach ($name in @(
      "RADIOIFY_NVIDIA_RTX_VIDEO_SDK_ARCHIVE",
      "NVIDIA_RTX_VIDEO_SDK_ARCHIVE",
      "RTX_VIDEO_SDK_ARCHIVE",
      "NGX_SDK_ARCHIVE")) {
    $value = Get-ProcessEnvironmentVariable -Name $name
    if ($value -and (Test-Path -LiteralPath $value)) {
      return (Get-Item -LiteralPath $value).FullName
    }
  }

  return $null
}

function Install-NvidiaRtxVideoSdkFromArchive {
  param(
    [Parameter(Mandatory = $true)]
    [pscustomobject]$Context,
    [Parameter(Mandatory = $true)]
    [string]$ArchivePath
  )

  $installRoot = Join-Path $Context.Paths.Root "third_party\nvidia\rtx-video-sdk"
  $repoRootFull = [System.IO.Path]::GetFullPath($Context.Paths.Root)
  $installRootFull = [System.IO.Path]::GetFullPath($installRoot)
  if (-not $installRootFull.StartsWith($repoRootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
    Fail-Build "Refusing to install NVIDIA RTX Video SDK outside the repository: $installRootFull"
  }

  if (Test-Path -LiteralPath $installRootFull) {
    Remove-Item -LiteralPath $installRootFull -Recurse -Force
  }
  New-Item -ItemType Directory -Force -Path $installRootFull | Out-Null

  Write-Host "Installing NVIDIA RTX Video SDK from archive: $ArchivePath"
  Expand-Archive -LiteralPath $ArchivePath -DestinationPath $installRootFull -Force

  $sdkRoot = Find-NvidiaRtxVideoSdkRootInTree -SearchRoot $installRootFull
  if (-not $sdkRoot) {
    Fail-Build @"
The NVIDIA RTX Video SDK archive was extracted, but no valid NGX SDK layout was found.

Expected files below one SDK root:
  include\nvsdk_ngx.h
  include\nvsdk_ngx_defs.h
  lib\x64\nvsdk_ngx_s.lib or lib\x64\nvsdk_ngx_d.lib

Archive: $ArchivePath
Extracted to: $installRootFull
"@
  }

  Set-ProcessEnvironmentVariable -Name "RADIOIFY_NVIDIA_RTX_VIDEO_SDK_ROOT" -Value $sdkRoot
  return $sdkRoot
}

function Assert-NvidiaRtxVideoSdkAvailable {
  param([pscustomobject]$Context)

  $sdkRoot = Resolve-NvidiaRtxVideoSdkRoot -RepoRoot $Context.Paths.Root
  if ($sdkRoot) {
    Set-ProcessEnvironmentVariable -Name "RADIOIFY_NVIDIA_RTX_VIDEO_SDK_ROOT" -Value $sdkRoot
    $Context.Build.NvidiaRtxVideoSdkRoot = $sdkRoot
    Write-Host "Using NVIDIA RTX Video SDK: $sdkRoot"
    return
  }

  Fail-Build @"
NVIDIA RTX Video SDK/NGX was explicitly enabled, but the SDK was not found.

Official route:
  1) Download the RTX Video SDK from:
     https://developer.nvidia.com/rtx-video-sdk
  2) Either install it normally, or rerun -InstallDeps with a downloaded SDK zip:
     `$env:RADIOIFY_NVIDIA_RTX_VIDEO_SDK_ARCHIVE="C:\path\to\RTX-Video-SDK.zip"
     .\build.ps1 -Static -EnableNvidiaRtxVideoSdk -InstallDeps

Alternative:
  Set RADIOIFY_NVIDIA_RTX_VIDEO_SDK_ROOT to the SDK root containing:
    include\nvsdk_ngx.h
    include\nvsdk_ngx_defs.h
    lib\x64\nvsdk_ngx_s.lib or nvsdk_ngx_d.lib

The SDK path is off by default. To enable it:
  .\build.ps1 -Static -EnableNvidiaRtxVideoSdk
"@
}

function Install-NvidiaRtxVideoSdkDependency {
  param([pscustomobject]$Context)

  $existingRoot = Resolve-NvidiaRtxVideoSdkRoot -RepoRoot $Context.Paths.Root
  if ($existingRoot) {
    Set-ProcessEnvironmentVariable -Name "RADIOIFY_NVIDIA_RTX_VIDEO_SDK_ROOT" -Value $existingRoot
    $Context.Build.NvidiaRtxVideoSdkRoot = $existingRoot
    Write-Host "NVIDIA RTX Video SDK already installed: $existingRoot"
    return
  }

  $archivePath = Get-NvidiaRtxVideoSdkArchivePath
  if ($archivePath) {
    $Context.Build.NvidiaRtxVideoSdkRoot =
      Install-NvidiaRtxVideoSdkFromArchive -Context $Context -ArchivePath $archivePath
    Write-Host "NVIDIA RTX Video SDK installed: $($Context.Build.NvidiaRtxVideoSdkRoot)"
    return
  }

  Assert-NvidiaRtxVideoSdkAvailable -Context $Context
}

function Configure-NvidiaRtxVideoSdkBuild {
  param([pscustomobject]$Context)

  if (-not $Context.Options.NvidiaRtxVideoSdk) {
    return
  }

  if ($Context.Options.InstallDeps) {
    Install-NvidiaRtxVideoSdkDependency -Context $Context
    return
  }

  Assert-NvidiaRtxVideoSdkAvailable -Context $Context
}

function Copy-NvidiaRtxVideoRuntime {
  param(
    [string]$SdkRoot,
    [string]$DistDir
  )

  $copiedArtifacts = New-Object System.Collections.Generic.List[string]
  if (-not $SdkRoot -or -not (Test-Path -LiteralPath $SdkRoot)) {
    return $copiedArtifacts
  }

  $featuresDir = Join-Path $SdkRoot "bin\features"
  if (-not (Test-Path -LiteralPath $featuresDir)) {
    Fail-Build "NVIDIA RTX Video SDK runtime DLL directory not found: $featuresDir"
  }

  if (-not (Test-Path -LiteralPath $DistDir)) {
    New-Item -ItemType Directory -Force -Path $DistDir | Out-Null
  }

  $runtimeDlls = Get-ChildItem -LiteralPath $featuresDir -Filter "nvngx_*.dll" -File |
    Sort-Object Name
  if (-not $runtimeDlls -or $runtimeDlls.Count -eq 0) {
    Fail-Build "NVIDIA RTX Video SDK runtime DLLs were not found in: $featuresDir"
  }

  foreach ($dll in $runtimeDlls) {
    $destination = Join-Path $DistDir $dll.Name
    Copy-Item -LiteralPath $dll.FullName -Destination $destination -Force
    [void]$copiedArtifacts.Add($destination)
  }

  return $copiedArtifacts
}
