function Get-ProcessEnvironmentVariable {
  param([Parameter(Mandatory = $true)][string]$Name)

  return [System.Environment]::GetEnvironmentVariable($Name, "Process")
}

function Set-ProcessEnvironmentVariable {
  param(
    [Parameter(Mandatory = $true)][string]$Name,
    [AllowNull()][string]$Value
  )

  [System.Environment]::SetEnvironmentVariable($Name, $Value, "Process")
}

function Add-UniqueEnvironmentListValue {
  param(
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][string]$Value,
    [string]$Separator = ";",
    [switch]$CaseInsensitive
  )

  $existing = Get-ProcessEnvironmentVariable -Name $Name
  $values = @()
  if ($existing) {
    foreach ($part in ($existing -split [regex]::Escape($Separator))) {
      if ($part) {
        $values += $part
      }
    }
  }

  foreach ($current in $values) {
    if ($CaseInsensitive) {
      if ([string]::Equals($current, $Value, [System.StringComparison]::OrdinalIgnoreCase)) {
        return
      }
    }
    elseif ($current -eq $Value) {
      return
    }
  }

  $values += $Value
  Set-ProcessEnvironmentVariable -Name $Name -Value ($values -join $Separator)
}

function Test-AnyEnvironmentVariableSet {
  param([string[]]$Names)

  foreach ($name in $Names) {
    if (Get-ProcessEnvironmentVariable -Name $name) {
      return $true
    }
  }

  return $false
}
