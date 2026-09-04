param(
    [Parameter(Mandatory = $true)]
    [string]$ApplicationPath,
    [Parameter(Mandatory = $true)]
    [string]$VideoPath,
    [ValidateRange(1, 240)]
    [int]$TimeoutMinutes = 60,
    [string]$ScratchRoot = [System.IO.Path]::GetTempPath(),
    [switch]$RequireGeneratedTranscript
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Stop-ProcessTree {
    param([Parameter(Mandatory = $true)][int]$ProcessId)

    $children = Get-CimInstance Win32_Process -Filter `
        "ParentProcessId = $ProcessId" -ErrorAction SilentlyContinue
    foreach ($child in $children) {
        Stop-ProcessTree -ProcessId ([int]$child.ProcessId)
    }
    Stop-Process -Id $ProcessId -Force -ErrorAction SilentlyContinue
}

function Quote-NativeArgument {
    param([Parameter(Mandatory = $true)][string]$Value)

    if ($Value.Contains('"')) {
        throw "Windows file paths containing a quotation mark are unsupported."
    }
    return '"' + $Value + '"'
}

function Invoke-HeadlessAnalysis {
    param(
        [Parameter(Mandatory = $true)][string]$Application,
        [Parameter(Mandatory = $true)][string]$Video,
        [Parameter(Mandatory = $true)][string]$CacheRoot,
        [Parameter(Mandatory = $true)][int]$TimeoutMilliseconds
    )

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $Application
    $startInfo.Arguments =
        "analyze-chapters " + (Quote-NativeArgument -Value $Video)
    $startInfo.WorkingDirectory = Split-Path -Parent $Application
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.EnvironmentVariables["RADIOIFY_CHAPTER_CACHE_ROOT"] = $CacheRoot

    $process = [System.Diagnostics.Process]::Start($startInfo)
    try {
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutMilliseconds)) {
            Stop-ProcessTree -ProcessId $process.Id
            throw "Chapter analysis exceeded the $TimeoutMinutes minute timeout."
        }
        # Complete asynchronous pipe draining after the process has exited.
        $process.WaitForExit()
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Stdout = $stdoutTask.Result
            Stderr = $stderrTask.Result
        }
    } finally {
        if (-not $process.HasExited) {
            Stop-ProcessTree -ProcessId $process.Id
        }
        $process.Dispose()
    }
}

function ConvertFrom-AnalysisResult {
    param(
        [Parameter(Mandatory = $true)]$ProcessResult,
        [Parameter(Mandatory = $true)][string]$RunName
    )

    if ($ProcessResult.ExitCode -ne 0) {
        throw "$RunName failed with exit code $($ProcessResult.ExitCode):`n" +
            "$($ProcessResult.Stdout)`n$($ProcessResult.Stderr)"
    }
    try {
        $document = $ProcessResult.Stdout | ConvertFrom-Json
    } catch {
        throw "$RunName did not return one valid JSON document:`n" +
            $ProcessResult.Stdout
    }
    if ($document.state -ne "ready" -or $document.chapters.Count -lt 1) {
        throw "$RunName returned no ready chapter analysis:`n" +
            $ProcessResult.Stdout
    }
    if (-not $document.persisted -or
        [string]::IsNullOrWhiteSpace([string]$document.cache_key) -or
        [string]::IsNullOrWhiteSpace([string]$document.cache_path) -or
        -not (Test-Path -LiteralPath $document.cache_path -PathType Leaf)) {
        throw "$RunName did not publish a durable cache artifact:`n" +
            $ProcessResult.Stdout
    }
    return $document
}

$resolvedApplication = [System.IO.Path]::GetFullPath($ApplicationPath)
$resolvedVideo = [System.IO.Path]::GetFullPath($VideoPath)
if (-not (Test-Path -LiteralPath $resolvedApplication -PathType Leaf)) {
    throw "Radioify executable not found: '$resolvedApplication'."
}
if (-not (Test-Path -LiteralPath $resolvedVideo -PathType Leaf)) {
    throw "Video input not found: '$resolvedVideo'."
}
$resolvedScratchRoot = [System.IO.Path]::GetFullPath($ScratchRoot)
if (-not (Test-Path -LiteralPath $resolvedScratchRoot -PathType Container)) {
    throw "E2E scratch root not found: '$resolvedScratchRoot'."
}

$timeoutMilliseconds = [int]($TimeoutMinutes * 60 * 1000)
$isolatedCacheRoot = Join-Path ([System.IO.Path]::GetTempPath()) `
    ("Radioify-Chapter-E2E-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $isolatedCacheRoot -ErrorAction Stop |
    Out-Null
$cacheItem = Get-Item -LiteralPath $isolatedCacheRoot -Force
if (($cacheItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "The newly created E2E cache root is a reparse point."
}

# A cache-only sandbox missed the production bug where analysis silently
# depended on a sibling transcript left by an earlier run. Materialize the
# media in a private scratch directory as well, preferring a zero-copy hard
# link where the source and scratch volumes support one.
$isolatedMediaRoot = Join-Path $resolvedScratchRoot `
    (".radioify-chapter-e2e-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $isolatedMediaRoot -ErrorAction Stop |
    Out-Null
$mediaItem = Get-Item -LiteralPath $isolatedMediaRoot -Force
if (($mediaItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "The newly created E2E media root is a reparse point."
}
$isolatedVideo = Join-Path $isolatedMediaRoot `
    ([System.IO.Path]::GetFileName($resolvedVideo))
$generatedTranscript = Join-Path $isolatedMediaRoot `
    ([System.IO.Path]::GetFileName($resolvedVideo) +
        ".radioify.transcript.en.srt")
$generatedProvenance = $generatedTranscript + ".radioify.json"
try {
    try {
        New-Item -ItemType HardLink -Path $isolatedVideo -Target $resolvedVideo `
            -ErrorAction Stop | Out-Null
        Write-Host "Created zero-copy isolated media fixture."
    } catch {
        Write-Host "Hard links are unavailable; copying the isolated media fixture..."
        Copy-Item -LiteralPath $resolvedVideo -Destination $isolatedVideo `
            -ErrorAction Stop
    }

    Write-Host "Running cold production chapter analysis without cache or sidecars..."
    $first = ConvertFrom-AnalysisResult `
        -ProcessResult (Invoke-HeadlessAnalysis `
            -Application $resolvedApplication `
            -Video $isolatedVideo `
            -CacheRoot $isolatedCacheRoot `
            -TimeoutMilliseconds $timeoutMilliseconds) `
        -RunName "Initial chapter analysis"

    if ($first.cache_hit) {
        throw "The isolated initial run unexpectedly resolved a warm cache."
    }
    if ($RequireGeneratedTranscript) {
        if (-not $first.english_text_evidence -or
            -not (Test-Path -LiteralPath $generatedTranscript -PathType Leaf) -or
            -not (Test-Path -LiteralPath $generatedProvenance -PathType Leaf)) {
            throw "The initial run did not publish its required English ASR sidecar and provenance record."
        }
    }
    $expectedCachePrefix =
        [System.IO.Path]::GetFullPath($isolatedCacheRoot).TrimEnd('\', '/') +
        [System.IO.Path]::DirectorySeparatorChar
    $reportedCachePath = [System.IO.Path]::GetFullPath($first.cache_path)
    if (-not $reportedCachePath.StartsWith(
            $expectedCachePrefix,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "The production analyzer ignored the isolated cache root."
    }

    Write-Host "Verifying deterministic discovery of the persisted result..."
    $second = ConvertFrom-AnalysisResult `
        -ProcessResult (Invoke-HeadlessAnalysis `
            -Application $resolvedApplication `
            -Video $isolatedVideo `
            -CacheRoot $isolatedCacheRoot `
            -TimeoutMilliseconds $timeoutMilliseconds) `
        -RunName "Cached chapter analysis"

    if (-not $second.cache_hit) {
        throw "The second production run did not resolve the durable cache."
    }
    if ($first.cache_key -ne $second.cache_key -or
        $first.cache_path -ne $second.cache_path) {
        throw "The production runs disagreed about chapter-cache identity."
    }

    Write-Host "Chapter E2E passed: $($second.chapters.Count) chapters"
    Write-Host "Verified isolated cold-to-warm cache publication."
} finally {
    $cacheItem = Get-Item -LiteralPath $isolatedCacheRoot -Force `
        -ErrorAction SilentlyContinue
    if ($cacheItem) {
        if (($cacheItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to clean an E2E cache root that became a reparse point."
        }
        Remove-Item -LiteralPath $isolatedCacheRoot -Recurse -Force
    }
    $mediaItem = Get-Item -LiteralPath $isolatedMediaRoot -Force `
        -ErrorAction SilentlyContinue
    if ($mediaItem) {
        if (($mediaItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to clean an E2E media root that became a reparse point."
        }
        Remove-Item -LiteralPath $isolatedMediaRoot -Recurse -Force
    }
}
