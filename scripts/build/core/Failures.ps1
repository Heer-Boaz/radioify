function Fail-Build {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Message,
    [int]$ExitCode = 1
  )

  $exception = [System.InvalidOperationException]::new($Message)
  $exception.Data["RadioifyExitCode"] = $ExitCode
  throw $exception
}
