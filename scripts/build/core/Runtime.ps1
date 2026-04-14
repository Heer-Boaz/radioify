function Enable-AnsiConsole {
  $runningOnWindows = $true
  if (Get-Variable -Name IsWindows -ErrorAction SilentlyContinue) {
    $runningOnWindows = [bool]$IsWindows
  }
  else {
    $runningOnWindows = ([System.Environment]::OSVersion.Platform -eq [System.PlatformID]::Win32NT)
  }

  if (-not $runningOnWindows) {
    return
  }

  if (-not ("Radioify.ConsoleMode" -as [type])) {
    Add-Type -Namespace Radioify -Name ConsoleMode -MemberDefinition @"
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)]
public static extern System.IntPtr GetStdHandle(int nStdHandle);

[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)]
[return: System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.Bool)]
public static extern bool GetConsoleMode(System.IntPtr hConsoleHandle, out uint lpMode);

[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)]
[return: System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.Bool)]
public static extern bool SetConsoleMode(System.IntPtr hConsoleHandle, uint dwMode);
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

function Prepend-PathDirectory {
  param([string]$PathToAdd)

  if (-not $PathToAdd) { return }
  $resolved = [System.IO.Path]::GetFullPath($PathToAdd)
  if (-not (Test-Path $resolved)) { return }

  $existingPath = Get-ProcessEnvironmentVariable -Name "PATH"
  $parts = @()
  if ($existingPath) {
    foreach ($part in ($existingPath -split ';')) {
      if (-not $part) { continue }
      if ([string]::Equals($part, $resolved, [System.StringComparison]::OrdinalIgnoreCase)) {
        continue
      }
      $parts += $part
    }
  }

  $newPath = ($resolved + $(if ($parts.Count -gt 0) { ";" + ($parts -join ';') } else { "" }))
  Set-ProcessEnvironmentVariable -Name "PATH" -Value $newPath
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
    "clang-cl.exe",
    "link.exe",
    "lld-link.exe",
    "powershell.exe",
    "pwsh.exe",
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
        Fail-Build "Could not delete path '$Path' after $MaxRetries retries: $($_.Exception.Message)"
      }

      Write-Warning "Delete attempt $attempt failed for '$Path'. Retrying in $DelayMilliseconds ms..."
      Start-Sleep -Milliseconds $DelayMilliseconds
    }
  }
}

function Invoke-CMakeConfigure {
  param(
    [string]$CMake,
    [string[]]$ArgumentList,
    [string]$WorkingDirectory,
    [string]$BuildDir,
    [pscustomobject]$Toolchain
  )

  $configureExit = Invoke-NativeProcess -FilePath $CMake -ArgumentList $ArgumentList -WorkingDirectory $WorkingDirectory
  if ($configureExit -eq 0) {
    return 0
  }

  if (-not $Toolchain.Ninja -or -not (Test-Path $BuildDir)) {
    return $configureExit
  }

  Write-Warning "CMake configure failed. Retrying once from a clean Ninja build directory."
  Stop-BuildProcessesUsingPath -TargetPath $BuildDir
  Remove-PathWithRetry -Path $BuildDir
  return (Invoke-NativeProcess -FilePath $CMake -ArgumentList $ArgumentList -WorkingDirectory $WorkingDirectory)
}
