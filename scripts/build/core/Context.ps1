function Resolve-TrailingSwitchOverrides {
  param([string[]]$RemainingArguments)

  $knownSwitches = @(
    "Clean",
    "Rebuild",
    "Static",
    "Ninja",
    "ClangCl",
    "VerboseBuild",
    "InstallDeps",
    "MelodyAnalysis",
    "Win11ExplorerIntegration",
    "TimingLog",
    "StagingUpload",
    "VideoErrorLog",
    "FfmpegErrorLog"
  )

  $overrides = [ordered]@{}
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
      $overrides[$normalized] = $true
      continue
    }

    $unexpected += $arg
  }

  if ($unexpected.Count -gt 0) {
    Fail-Build ("Unknown arguments: {0}" -f ($unexpected -join ", "))
  }

  return $overrides
}

function New-InitialBuildOptions {
  param(
    [string]$Config,
    [bool]$Clean,
    [bool]$Rebuild,
    [bool]$Static,
    [bool]$Ninja,
    [bool]$ClangCl,
    [bool]$ClangClExplicitlySet,
    [bool]$VerboseBuild,
    [bool]$InstallDeps,
    [string]$VcpkgRoot,
    [bool]$MelodyAnalysis,
    [bool]$Win11ExplorerIntegration,
    [bool]$TimingLog,
    [bool]$StagingUpload,
    [bool]$VideoErrorLog,
    [bool]$FfmpegErrorLog
  )

  return [ordered]@{
    Config = $Config
    Clean = $Clean
    Rebuild = $Rebuild
    Static = $Static
    Ninja = $Ninja
    ClangCl = $ClangCl
    ClangClExplicitlySet = $ClangClExplicitlySet
    VerboseBuild = $VerboseBuild
    InstallDeps = $InstallDeps
    VcpkgRoot = $VcpkgRoot
    MelodyAnalysis = $MelodyAnalysis
    Win11ExplorerIntegration = $Win11ExplorerIntegration
    TimingLog = $TimingLog
    StagingUpload = $StagingUpload
    VideoErrorLog = $VideoErrorLog
    FfmpegErrorLog = $FfmpegErrorLog
  }
}

function New-BuildContext {
  param(
    [string]$RepoRoot,
    [System.Collections.IDictionary]$InitialOptions,
    [string[]]$RemainingArguments = @()
  )

  $optionsData = [ordered]@{}
  foreach ($entry in $InitialOptions.GetEnumerator()) {
    $optionsData[$entry.Key] = $entry.Value
  }

  $overrides = Resolve-TrailingSwitchOverrides -RemainingArguments $RemainingArguments
  foreach ($entry in $overrides.GetEnumerator()) {
    $optionsData[$entry.Key] = $entry.Value
  }

  return [pscustomobject]@{
    Options = [pscustomobject]$optionsData
    Paths = [pscustomobject]@{
      Root = $RepoRoot
      DistDir = Join-Path $RepoRoot "dist"
      BuildDir = $null
    }
    Tools = [pscustomobject]@{
      CMake = $null
      VcpkgRoot = $null
      VcpkgExe = $null
      Toolchain = $null
    }
    Build = [pscustomobject]@{
      TripletInfo = $null
      InstalledRoot = $null
      ConfigureInfo = $null
      FfmpegTripletDir = $null
    }
  }
}
