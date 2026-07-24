Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "RadioifyWindowsMsixCommon.ps1")

function Resolve-RadioifyMsixAppxCmdlet {
    param([Parameter(Mandatory = $true)][string]$Name)

    $command = Get-Command -Name $Name -CommandType Cmdlet -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) {
        return $command
    }

    Import-Module Appx -DisableNameChecking -ErrorAction Stop
    $command = Get-Command -Name $Name -CommandType Cmdlet -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) {
        return $command
    }

    throw "Required Appx cmdlet '$Name' was not found."
}

function Resolve-RadioifyWindowsPowerShellExecutable {
    $systemRoot = [Environment]::GetEnvironmentVariable("SystemRoot")
    if (-not [string]::IsNullOrWhiteSpace($systemRoot)) {
        $candidate = Join-Path $systemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    $command = Get-Command powershell.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command -and $command.Source) {
        return $command.Source
    }

    throw "Windows PowerShell was not found. MSIX deployment requires the Windows Appx PowerShell module."
}

function Invoke-RadioifyMsixDeploymentPowerShell {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$ScriptBlock,
        [hashtable]$Parameters,
        [Parameter(Mandatory = $true)][string]$OperationName,
        [int]$TimeoutSeconds = 180
    )

    $tempRoot = [System.IO.Path]::GetTempPath()
    $operationId = [guid]::NewGuid().ToString("N")
    $payloadPath = Join-Path $tempRoot "radioify-appx-$operationId.json"
    $payloadLiteral = $payloadPath.Replace("'", "''")

    $payload = [ordered]@{
        Script = $ScriptBlock.ToString()
        Parameters = if ($Parameters) { $Parameters } else { @{} }
    }
    $payload | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $payloadPath -Encoding UTF8

    $bootstrap = @"
`$ErrorActionPreference = "Stop"
`$ProgressPreference = "SilentlyContinue"
`$payload = Get-Content -LiteralPath '$payloadLiteral' -Raw | ConvertFrom-Json
`$params = @{}
if (`$payload.Parameters) {
    foreach (`$entry in `$payload.Parameters.PSObject.Properties) {
        `$params[`$entry.Name] = `$entry.Value
    }
}
`$scriptBlock = [ScriptBlock]::Create([string]`$payload.Script)
& `$scriptBlock @params
"@
    $encodedCommand = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($bootstrap))
    $powershellExe = Resolve-RadioifyWindowsPowerShellExecutable

    $process = $null
    try {
        $startInfo = New-Object System.Diagnostics.ProcessStartInfo
        $startInfo.FileName = $powershellExe
        $startInfo.Arguments = "-NoProfile -ExecutionPolicy Bypass -EncodedCommand $encodedCommand"
        $startInfo.UseShellExecute = $false
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $startInfo.CreateNoWindow = $true

        $process = [System.Diagnostics.Process]::Start($startInfo)
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()

        $startedAt = Get-Date
        $deadline = $startedAt.AddSeconds($TimeoutSeconds)
        $lastStatusAt = $startedAt
        $completed = $false
        while (-not $completed) {
            $completed = $process.WaitForExit(1000)
            if ($completed) {
                break
            }

            $now = Get-Date
            if ($now -ge $deadline) {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
                throw "$OperationName did not finish within $TimeoutSeconds seconds. The installer stopped waiting instead of deadlocking."
            }

            if (($now - $lastStatusAt).TotalSeconds -ge 10) {
                $elapsedSeconds = [int][Math]::Floor(($now - $startedAt).TotalSeconds)
                Write-Host "$OperationName is still running... ${elapsedSeconds}s elapsed"
                $lastStatusAt = $now
            }
        }

        $process.WaitForExit()
        $stdout = $stdoutTask.Result
        $stderr = $stderrTask.Result

        if ($process.ExitCode -ne 0) {
            $details = (($stderr, $stdout) |
                Where-Object { -not [string]::IsNullOrWhiteSpace($_) }) -join [Environment]::NewLine
            if ($details) {
                throw "$OperationName failed with exit code $($process.ExitCode).`n$details"
            }
            throw "$OperationName failed with exit code $($process.ExitCode)."
        }

        if (-not [string]::IsNullOrWhiteSpace($stdout)) {
            $trimmedStdout = $stdout.Trim()
            return $trimmedStdout
        }

        return ""
    } finally {
        if ($process) {
            $process.Dispose()
        }
        Remove-Item -LiteralPath $payloadPath -Force -ErrorAction SilentlyContinue
    }
}

function Get-RadioifyMsixCertificateThumbprint {
    param([Parameter(Mandatory = $true)][string]$CertificatePath)

    if (-not (Test-Path -LiteralPath $CertificatePath)) {
        throw "MSIX certificate not found at '$CertificatePath'."
    }

    $certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($CertificatePath)
    try {
        return $certificate.Thumbprint
    } finally {
        $certificate.Dispose()
    }
}

function Test-RadioifyMsixCertificateTrusted {
    param([Parameter(Mandatory = $true)][string]$CertificatePath)

    $thumbprint = Get-RadioifyMsixCertificateThumbprint -CertificatePath $CertificatePath
    $result = Invoke-RadioifyMsixDeploymentPowerShell `
        -OperationName "Check Radioify MSIX certificate trust" `
        -TimeoutSeconds 30 `
        -ScriptBlock {
            param([Parameter(Mandatory = $true)][string]$Thumbprint)

            $store = [System.Security.Cryptography.X509Certificates.X509Store]::new(
                [System.Security.Cryptography.X509Certificates.StoreName]::TrustedPeople,
                [System.Security.Cryptography.X509Certificates.StoreLocation]::LocalMachine)
            try {
                $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadOnly)
                $matches = $store.Certificates.Find(
                    [System.Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint,
                    $Thumbprint,
                    $false)
                if ($matches.Count -gt 0) {
                    Write-Output "trusted"
                } else {
                    Write-Output "missing"
                }
            } finally {
                $store.Close()
            }
        } `
        -Parameters @{
            Thumbprint = $thumbprint
        }

    return ($result -eq "trusted")
}

function Ensure-RadioifyMsixCertificateTrusted {
    param(
        [Parameter(Mandatory = $true)][string]$CertificatePath
    )

    if (-not (Test-Path -LiteralPath $CertificatePath)) {
        throw "MSIX certificate not found at '$CertificatePath'."
    }
    if (Test-RadioifyMsixCertificateTrusted -CertificatePath $CertificatePath) {
        return
    }

    if (-not (Test-RadioifyAdministrator)) {
        throw @"
Installing the Radioify MSIX package requires an elevated PowerShell session.

The self-signed MSIX package certificate must be trusted in:
  Cert:\LocalMachine\TrustedPeople

Re-run:
  powershell -ExecutionPolicy Bypass -File .\install_radioify.ps1

from an elevated PowerShell window.
"@
    }

    $thumbprint = Get-RadioifyMsixCertificateThumbprint -CertificatePath $CertificatePath
    Write-Host "Trusting MSIX signing certificate: $thumbprint"
    Invoke-RadioifyMsixDeploymentPowerShell `
        -OperationName "Trust Radioify MSIX certificate" `
        -TimeoutSeconds 30 `
        -ScriptBlock {
            param(
                [Parameter(Mandatory = $true)][string]$CertificatePath,
                [Parameter(Mandatory = $true)][string]$Thumbprint
            )

            $certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($CertificatePath)
            if ($certificate.Thumbprint -ne $Thumbprint) {
                throw "Certificate thumbprint changed while importing '$CertificatePath'."
            }

            $store = [System.Security.Cryptography.X509Certificates.X509Store]::new(
                [System.Security.Cryptography.X509Certificates.StoreName]::TrustedPeople,
                [System.Security.Cryptography.X509Certificates.StoreLocation]::LocalMachine)
            try {
                $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
                $matches = $store.Certificates.Find(
                    [System.Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint,
                    $Thumbprint,
                    $false)
                if ($matches.Count -eq 0) {
                    $store.Add($certificate)
                }
            } finally {
                $store.Close()
                $certificate.Dispose()
            }
        } `
        -Parameters @{
            CertificatePath = $CertificatePath
            Thumbprint = $thumbprint
        }
}

function Ensure-RadioifyPackagedMsixTrusted {
    param([Parameter(Mandatory = $true)][string]$IntegrationDir)

    $manifestInfo = Get-RadioifyMsixManifestInfo -IntegrationDir $IntegrationDir
    $certificatePath = Get-RadioifyMsixCertificatePath `
        -IntegrationDir $IntegrationDir `
        -PackageName $manifestInfo.PackageName
    if (-not (Test-Path -LiteralPath $certificatePath)) {
        throw "MSIX certificate not found at '$certificatePath'. Repackage Radioify so the signed .msix and .cer are bundled together."
    }

    Ensure-RadioifyMsixCertificateTrusted -CertificatePath $certificatePath
    return $certificatePath
}

function Get-InstalledRadioifyMsixPackage {
    param([Parameter(Mandatory = $true)][string]$PackageName)

    $cmdlet = Resolve-RadioifyMsixAppxCmdlet -Name "Get-AppxPackage"
    return (& $cmdlet -Name $PackageName -ErrorAction SilentlyContinue | Select-Object -First 1)
}

function Get-RadioifyMsixPackageProcesses {
    param([Parameter(Mandatory = $true)]$Package)

    $installLocation = [string]$Package.InstallLocation
    if ([string]::IsNullOrWhiteSpace($installLocation)) {
        throw "Installed Radioify package '$($Package.PackageFullName)' has no install location."
    }

    $radioifyExePath = Join-Path $installLocation "radioify.exe"
    $explorerDllPath = Join-Path $installLocation "radioify_explorer.dll"
    $matches = @{}

    foreach ($process in Get-CimInstance Win32_Process -Filter "Name='radioify.exe'" -ErrorAction SilentlyContinue) {
        if ([string]::Equals(
                [string]$process.ExecutablePath,
                $radioifyExePath,
                [StringComparison]::OrdinalIgnoreCase)) {
            $matches[[int]$process.ProcessId] = [pscustomobject]@{
                ProcessId = [int]$process.ProcessId
                Name = [string]$process.Name
                Role = "application"
                ImagePath = [string]$process.ExecutablePath
            }
        }
    }

    foreach ($process in Get-Process -Name "dllhost" -ErrorAction SilentlyContinue) {
        try {
            $module = $process.Modules |
                Where-Object {
                    [string]::Equals(
                        $_.FileName,
                        $explorerDllPath,
                        [StringComparison]::OrdinalIgnoreCase)
                } |
                Select-Object -First 1
            if ($module) {
                $matches[[int]$process.Id] = [pscustomobject]@{
                    ProcessId = [int]$process.Id
                    Name = [string]$process.ProcessName
                    Role = "Explorer COM surrogate"
                    ImagePath = [string]$module.FileName
                }
            }
        } catch {
            Write-Verbose "Could not inspect modules for dllhost PID $($process.Id): $($_.Exception.Message)"
        }
    }

    return @($matches.Values | Sort-Object ProcessId)
}

function Stop-RadioifyMsixPackageProcesses {
    param(
        [Parameter(Mandatory = $true)]$Package,
        [int]$TimeoutSeconds = 10
    )

    $processes = @(Get-RadioifyMsixPackageProcesses -Package $Package)
    if ($processes.Count -eq 0) {
        return
    }

    foreach ($process in $processes) {
        Write-Host "Stopping installed Radioify $($process.Role) process PID $($process.ProcessId)..."
        try {
            Stop-Process -Id $process.ProcessId -Force -ErrorAction Stop
        } catch {
            if (Get-Process -Id $process.ProcessId -ErrorAction SilentlyContinue) {
                throw
            }
        }
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $remaining = @(Get-RadioifyMsixPackageProcesses -Package $Package)
        if ($remaining.Count -eq 0) {
            return
        }
        Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $deadline)

    $descriptions = $remaining |
        ForEach-Object { "$($_.Name) PID $($_.ProcessId) ($($_.Role))" }
    throw "Timed out stopping installed Radioify package processes: $($descriptions -join ', ')"
}

function Test-RadioifyMsixPackageBusy {
    param([Parameter(Mandatory = $true)]$Package)

    if (-not $Package) {
        return $false
    }

    $statusText = [string]$Package.Status
    return ($statusText -match 'DeploymentInProgress|Servicing')
}

function Wait-RadioifyMsixPackageState {
    param(
        [Parameter(Mandatory = $true)][string]$PackageName,
        [Parameter(Mandatory = $true)][ValidateSet("Absent", "Ready")][string]$DesiredState,
        [int]$TimeoutSeconds = 30
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $package = Get-InstalledRadioifyMsixPackage -PackageName $PackageName
        if ($DesiredState -eq "Absent") {
            if (-not $package) {
                return
            }
        } else {
            if ($package -and -not (Test-RadioifyMsixPackageBusy -Package $package)) {
                return
            }
        }

        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $deadline)

    $package = Get-InstalledRadioifyMsixPackage -PackageName $PackageName
    if ($DesiredState -eq "Absent") {
        $statusText = if ($package) { [string]$package.Status } else { "present" }
        throw "Timed out waiting for package '$PackageName' to be removed. Current status: $statusText"
    }

    $statusText = if ($package) { [string]$package.Status } else { "not installed" }
    throw "Timed out waiting for package '$PackageName' to become ready. Current status: $statusText"
}

function Install-RadioifyMsixPackage {
    param(
        [Parameter(Mandatory = $true)][string]$PackagePath
    )

    Invoke-RadioifyMsixDeploymentPowerShell `
        -OperationName "Install Radioify MSIX package" `
        -TimeoutSeconds 180 `
        -ScriptBlock {
            param([Parameter(Mandatory = $true)][string]$PackagePath)

            Add-AppxPackage `
                -Path $PackagePath `
                -ForceTargetApplicationShutdown `
                -ForceUpdateFromAnyVersion `
                -ErrorAction Stop
        } `
        -Parameters @{
            PackagePath = $PackagePath
        }
}

function Remove-RadioifyMsixPackage {
    param(
        [Parameter(Mandatory = $true)][string]$PackageFullName,
        [string]$PackageName
    )

    try {
        Invoke-RadioifyMsixDeploymentPowerShell `
            -OperationName "Remove Radioify MSIX package" `
            -TimeoutSeconds 180 `
            -ScriptBlock {
                param([Parameter(Mandatory = $true)][string]$PackageFullName)

                Remove-AppxPackage `
                    -Package $PackageFullName `
                    -ErrorAction Stop
            } `
            -Parameters @{
                PackageFullName = $PackageFullName
            }
    } catch {
        if (-not [string]::IsNullOrWhiteSpace($PackageName)) {
            $remainingPackage = Get-InstalledRadioifyMsixPackage -PackageName $PackageName
            if (-not $remainingPackage) {
                Write-Warning "Remove-AppxPackage reported an error after '$PackageFullName' was already removed. Continuing."
                return
            }
        }

        throw
    }
}
