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
  [string]$VcpkgRoot,
  [switch]$MelodyAnalysis,
  [switch]$Win11ExplorerIntegration,
  [switch]$TimingLog,
  [switch]$StagingUpload,
  [switch]$VideoErrorLog,
  [switch]$FfmpegErrorLog,
  [switch]$DisableNvidiaRtxVideo
)

$ErrorActionPreference = "Stop"

$buildRoot = Join-Path $PSScriptRoot "scripts\build"
. (Join-Path $buildRoot "Bootstrap.ps1")
foreach ($modulePath in (Resolve-BuildModulePaths -BuildRoot $buildRoot)) {
  . $modulePath
}

try {
  Enable-AnsiConsole

  $context = New-BuildContext `
    -RepoRoot $PSScriptRoot `
    -InitialOptions (New-InitialBuildOptions `
      -Config $Config `
      -Clean ([bool]$Clean) `
      -Rebuild ([bool]$Rebuild) `
      -Static ([bool]$Static) `
      -Ninja ([bool]$Ninja) `
      -ClangCl ([bool]$ClangCl) `
      -ClangClExplicitlySet $PSBoundParameters.ContainsKey("ClangCl") `
      -VerboseBuild ([bool]$VerboseBuild) `
      -InstallDeps ([bool]$InstallDeps) `
      -VcpkgRoot $VcpkgRoot `
      -MelodyAnalysis ([bool]$MelodyAnalysis) `
      -Win11ExplorerIntegration ([bool]$Win11ExplorerIntegration) `
      -TimingLog ([bool]$TimingLog) `
      -StagingUpload ([bool]$StagingUpload) `
      -VideoErrorLog ([bool]$VideoErrorLog) `
      -FfmpegErrorLog ([bool]$FfmpegErrorLog) `
      -DisableNvidiaRtxVideo ([bool]$DisableNvidiaRtxVideo)) `
    -RemainingArguments $args

  $buildExit = Invoke-RadioifyBuild -Context $context
  exit $buildExit
}
catch {
  $message = $_.Exception.Message
  if (-not $message) {
    $message = $_.ToString()
  }
  Write-Error $message

  $exitCode = 1
  if ($_.Exception.Data.Contains("RadioifyExitCode")) {
    $exitCode = [int]$_.Exception.Data["RadioifyExitCode"]
  }

  exit $exitCode
}
