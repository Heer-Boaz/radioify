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

function Invoke-NativeProcessWithOutput {
  param(
    [Parameter(Mandatory = $true)]
    [string]$FilePath,
    [string[]]$ArgumentList = @(),
    [string]$WorkingDirectory = $null
  )

  $startInfo = New-Object System.Diagnostics.ProcessStartInfo
  $startInfo.FileName = $FilePath
  $startInfo.UseShellExecute = $false
  $startInfo.RedirectStandardOutput = $true
  $startInfo.RedirectStandardError = $true
  $startInfo.CreateNoWindow = $true

  $formattedArgumentList = Join-WindowsCommandLineArguments -ArgumentList $ArgumentList
  if ($formattedArgumentList) {
    $startInfo.Arguments = $formattedArgumentList
  }
  if ($WorkingDirectory) {
    $startInfo.WorkingDirectory = $WorkingDirectory
  }

  $process = New-Object System.Diagnostics.Process
  $process.StartInfo = $startInfo
  [void]$process.Start()
  $stdout = $process.StandardOutput.ReadToEnd()
  $stderr = $process.StandardError.ReadToEnd()
  $process.WaitForExit()

  if ($stdout) {
    [Console]::Out.Write($stdout)
  }
  if ($stderr) {
    [Console]::Error.Write($stderr)
  }

  return [int]$process.ExitCode
}
