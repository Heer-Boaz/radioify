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
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Required staged-runtime source is missing: '$Source'."
    }
    $destinationParent = Split-Path -Parent $Destination
    if (-not (Test-Path -LiteralPath $destinationParent -PathType Container)) {
        New-Item -ItemType Directory -Path $destinationParent -Force | Out-Null
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function Invoke-StagedRadioify {
    param([Parameter(Mandatory = $true)][string]$Arguments)

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $script:stagedApplication
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
        $process.WaitForExit()
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
$stageName = ".radioify-chapter-runtime-$PID-$([Guid]::NewGuid().ToString('N'))"
$resolvedStage = Join-Path $sourceDirectory $stageName
New-Item -ItemType Directory -Path $resolvedStage -ErrorAction Stop | Out-Null
$stageItem = Get-Item -LiteralPath $resolvedStage -Force
if (($stageItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "The newly created runtime test stage is a reparse point."
}

try {
    $applicationName = Split-Path -Leaf $resolvedApplication
    Copy-RequiredFile -Source $resolvedApplication `
        -Destination (Join-Path $resolvedStage $applicationName)
    foreach ($fileName in @("radioify_chapter_worker.exe", "llama-cpp-LICENSE.txt")) {
        Copy-RequiredFile -Source (Join-Path $sourceDirectory $fileName) `
            -Destination (Join-Path $resolvedStage $fileName)
    }

    $chapterRelativeDirectory = "models\chapter_analysis"
    foreach ($fileName in @(
        "chapter-llama-asr-10k-f16.gguf",
        "chapter-llama-captions-asr-10k-f16.gguf",
        "CHAPTER-LLAMA-NOTICE.md",
        "LLAMA-3.1-LICENSE",
        "NOTICE"
    )) {
        Copy-RequiredFile `
            -Source (Join-Path $sourceDirectory "$chapterRelativeDirectory\$fileName") `
            -Destination (Join-Path $resolvedStage "$chapterRelativeDirectory\$fileName")
    }
    $runtimeContract = Get-RadioifyWindowsMlRuntimeContract
    if ($WindowsMlRuntime -eq "ON") {
        foreach ($fileName in $runtimeContract.ProductionRuntimeFiles) {
            Copy-RequiredFile -Source (Join-Path $sourceDirectory $fileName) `
                -Destination (Join-Path $resolvedStage $fileName)
        }
    }
    foreach ($fileName in $runtimeContract.DiagnosticOnlyRuntimeFiles) {
        if (Test-Path -LiteralPath (Join-Path $resolvedStage $fileName)) {
            throw "Diagnostic-only runtime leaked into the production stage: '$fileName'."
        }
    }

    $stagedApplication = Join-Path $resolvedStage $applicationName
    $runtimeVerification = Invoke-StagedRadioify -Arguments "--verify-chapter-runtime"
    $stdout = $runtimeVerification.Stdout
    $stderr = $runtimeVerification.Stderr

    if (-not [string]::IsNullOrWhiteSpace($stderr)) {
        Write-Host $stderr.TrimEnd()
    }
    if ($runtimeVerification.ExitCode -ne 0) {
        throw "The staged chapter runtime rejected its own layout:`n$stdout"
    }

    $reported = @{}
    foreach ($line in ($stdout -split "`r?`n")) {
        $separator = $line.IndexOf('=')
        if ($separator -le 0) { continue }
        $reported[$line.Substring(0, $separator)] = $line.Substring($separator + 1)
    }
    if ($reported["state"] -ne "ready") {
        throw "The staged chapter runtime did not report state=ready:`n$stdout"
    }

    $expectedRoot = [System.IO.Path]::GetFullPath($resolvedStage)
    $expectedSpeechPlanAdapter = [System.IO.Path]::GetFullPath((Join-Path $resolvedStage `
        "$chapterRelativeDirectory\chapter-llama-asr-10k-f16.gguf"))
    $expectedChapterPlanAdapter = [System.IO.Path]::GetFullPath((Join-Path $resolvedStage `
        "$chapterRelativeDirectory\chapter-llama-captions-asr-10k-f16.gguf"))
    if (-not $reported.ContainsKey("executable_root") -or
        -not $expectedRoot.Equals(
            [System.IO.Path]::GetFullPath($reported["executable_root"]),
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "The verifier did not resolve the isolated staged executable root:`n$stdout"
    }
    if (-not $reported.ContainsKey("speech_plan_adapter") -or
        -not $expectedSpeechPlanAdapter.Equals(
            [System.IO.Path]::GetFullPath($reported["speech_plan_adapter"]),
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "The verifier escaped the isolated stage while resolving its ASR plan adapter:`n$stdout"
    }
    if (-not $reported.ContainsKey("chapter_plan_adapter") -or
        -not $expectedChapterPlanAdapter.Equals(
            [System.IO.Path]::GetFullPath($reported["chapter_plan_adapter"]),
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "The verifier escaped the isolated stage while resolving its captions-plus-ASR adapter:`n$stdout"
    }

# Exercise command routing and the machine-readable result contract from the
# same isolated layout. A missing input terminates before GPU/model work and is
# therefore deterministic on developer machines and CI.
    $missingInput = "__radioify_missing_chapter_smoke__.webm"
    $cliSmoke = Invoke-StagedRadioify `
        -Arguments ('analyze-chapters "{0}"' -f $missingInput)
    if ($cliSmoke.ExitCode -ne 3) {
        throw "The staged headless analyzer returned an unexpected exit code " +
            "($($cliSmoke.ExitCode)):`n$($cliSmoke.Stdout)`n$($cliSmoke.Stderr)"
    }
    try {
        $cliDocument = $cliSmoke.Stdout | ConvertFrom-Json
    } catch {
        throw "The staged headless analyzer did not return valid JSON:`n" +
            $cliSmoke.Stdout
    }
    if ($cliDocument.state -ne "unsupported" -or
        [string]::IsNullOrWhiteSpace([string]$cliDocument.detail)) {
        throw "The staged headless analyzer violated its failure contract:`n" +
            $cliSmoke.Stdout
    }

    Write-Host "Verified isolated chapter runtime layout: $resolvedStage"
} finally {
    $stageItem = Get-Item -LiteralPath $resolvedStage -Force -ErrorAction SilentlyContinue
    if ($stageItem) {
        if (($stageItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to clean a runtime test stage that became a reparse point."
        }
        Remove-Item -LiteralPath $resolvedStage -Recurse -Force
    }
}
