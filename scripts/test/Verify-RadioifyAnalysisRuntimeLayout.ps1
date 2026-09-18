param(
    [Parameter(Mandatory = $true)]
    [string]$ApplicationPath,
    [ValidateSet("ON", "OFF")]
    [string]$WindowsMlRuntime = "OFF"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "..\windows\RadioifyWindowsRuntimeContract.ps1")

function Copy-RequiredFile {
    param([string]$Source, [string]$Destination)

    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Required staged-runtime source is missing: '$Source'."
    }
    Copy-Item -LiteralPath $Source -Destination $Destination
}

function Invoke-StagedProgram {
    param([string]$Executable, [string]$Arguments = "")

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $Executable
    $startInfo.Arguments = $Arguments
    $startInfo.WorkingDirectory = $script:resolvedStage
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::Start($startInfo)
    try {
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(30000)) {
            $process.Kill()
            throw "The staged program did not terminate: '$Executable'."
        }
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Stdout = $stdoutTask.Result
            Stderr = $stderrTask.Result
        }
    } finally {
        $process.Dispose()
    }
}

$resolvedApplication = [System.IO.Path]::GetFullPath($ApplicationPath)
$sourceDirectory = Split-Path -Parent $resolvedApplication
$stageName = ".radioify-analysis-runtime-$PID-$([Guid]::NewGuid().ToString('N'))"
$resolvedStage = [System.IO.Path]::GetFullPath((Join-Path $sourceDirectory $stageName))
New-Item -ItemType Directory -Path $resolvedStage -ErrorAction Stop | Out-Null

try {
    $stageItem = Get-Item -LiteralPath $resolvedStage -Force
    if (($stageItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "The newly created runtime test stage is a reparse point."
    }
    $applicationName = Split-Path -Leaf $resolvedApplication
    Copy-RequiredFile -Source $resolvedApplication -Destination (Join-Path $resolvedStage $applicationName)
    foreach ($fileName in @("radioify_analysis_worker.exe", "llama-cpp-LICENSE.txt")) {
        Copy-RequiredFile -Source (Join-Path $sourceDirectory $fileName) -Destination (Join-Path $resolvedStage $fileName)
    }
    $runtimeContract = Get-RadioifyWindowsMlRuntimeContract
    if ($WindowsMlRuntime -eq "ON") {
        foreach ($fileName in $runtimeContract.ProductionRuntimeFiles) {
            Copy-RequiredFile -Source (Join-Path $sourceDirectory $fileName) -Destination (Join-Path $resolvedStage $fileName)
        }
    }
    foreach ($fileName in $runtimeContract.DiagnosticOnlyRuntimeFiles) {
        if (Test-Path -LiteralPath (Join-Path $resolvedStage $fileName)) {
            throw "Diagnostic-only runtime leaked into the production stage: '$fileName'."
        }
    }

    # Neither smoke check starts playback, downloads models, or performs inference.
    $help = Invoke-StagedProgram -Executable (Join-Path $resolvedStage $applicationName) -Arguments "--help"
    if ($help.ExitCode -ne 0) {
        throw "The staged application failed to start: $($help.ExitCode) $($help.Stderr)"
    }
    if (($help.Stdout + $help.Stderr) -match "analyze-chapters|verify-chapter|automatic-chapters") {
        throw "The application still advertises the retired playback-analysis commands."
    }
    $worker = Invoke-StagedProgram -Executable (Join-Path $resolvedStage "radioify_analysis_worker.exe")
    if ($worker.ExitCode -ne 1) {
        throw "The staged analysis worker failed its no-arguments check: $($worker.ExitCode) $($worker.Stderr)"
    }
    Write-Host "Verified isolated editing-analysis runtime layout: $resolvedStage"
} finally {
    $stageItem = Get-Item -LiteralPath $resolvedStage -Force -ErrorAction SilentlyContinue
    if ($stageItem) {
        if (-not $sourceDirectory.Equals((Split-Path -Parent $resolvedStage), [System.StringComparison]::OrdinalIgnoreCase) -or
            ($stageItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to clean a runtime test stage outside its expected parent or through a reparse point."
        }
        Remove-Item -LiteralPath $resolvedStage -Recurse -Force
    }
}
