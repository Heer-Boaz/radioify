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
    Fail-Build @"
ONNX overlay port is incomplete. Missing required files:
$($missing -join "`n")

Restore these files in: $onnxPortDir
"@
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
      Fail-Build @"
ONNX is currently built with ONNX_DISABLE_STATIC_REGISTRATION=OFF.
That causes Melody mode schema spam/crashes with ONNX Runtime static builds.

Fix:
  1) Delete this folder: $InstalledRoot
  2) Re-run: .\\build.ps1 -Static

This build now ships an ONNX overlay port that forces ONNX_DISABLE_STATIC_REGISTRATION=ON.
"@
    }
  }
}

function Configure-MelodyAnalysisBuild {
  param([pscustomobject]$Context)

  if (-not $Context.Options.MelodyAnalysis) {
    return
  }

  $overlayPortsDir = Join-Path $Context.Paths.Root "vcpkg-overlays\ports"
  if (Test-Path $overlayPortsDir) {
    Assert-OnnxOverlayPortComplete $overlayPortsDir
    Add-VcpkgOverlayPortPath $overlayPortsDir
    Write-Host "Using vcpkg overlay ports: $overlayPortsDir"
  }
}
