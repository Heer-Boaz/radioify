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
  if ($BuildDir) {
    $candidates += (Join-Path $BuildDir "bin\radioify.exe")
    if ($Config) {
      $candidates += (Join-Path (Join-Path $BuildDir "bin\$Config") "radioify.exe")
    }
    $candidates += (Join-Path $BuildDir "radioify.exe")
    if ($Config) {
      $candidates += (Join-Path (Join-Path $BuildDir $Config) "radioify.exe")
    }
  }
  if ($Root) {
    $candidates += (Join-Path $Root "dist\radioify.exe")
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

function Try-Copy-BuildArtifact {
  param(
    [Parameter(Mandatory = $true)]
    [string]$SourcePath,
    [Parameter(Mandatory = $true)]
    [string]$DestinationPath,
    [int]$MaxRetries = 3,
    [int]$DelayMilliseconds = 250
  )

  $destinationDir = Split-Path -Parent $DestinationPath
  if ($destinationDir -and -not (Test-Path $destinationDir)) {
    New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null
  }

  for ($attempt = 1; $attempt -le $MaxRetries; $attempt++) {
    try {
      Copy-Item -Force -Path $SourcePath -Destination $DestinationPath -ErrorAction Stop
      return [pscustomobject]@{
        Success = $true
        ErrorMessage = $null
      }
    }
    catch {
      $errorMessage = $_.Exception.Message
      if ($attempt -lt $MaxRetries) {
        Start-Sleep -Milliseconds $DelayMilliseconds
        continue
      }

      return [pscustomobject]@{
        Success = $false
        ErrorMessage = $errorMessage
      }
    }
  }
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
