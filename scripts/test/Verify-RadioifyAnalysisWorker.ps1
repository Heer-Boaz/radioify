param([Parameter(Mandatory = $true)][string]$Executable)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$root = Join-Path ([IO.Path]::GetTempPath()) ("radioify-worker-test-" + [guid]::NewGuid().ToString('N') + ' space-' + [char]0x96ea)
[IO.Directory]::CreateDirectory($root) | Out-Null
function Invoke-Worker {
    param([string]$Arguments, [int]$ExpectedCode)
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $Executable
    $start.Arguments = $Arguments
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($start)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(15000)) {
            $process.Kill()
            throw 'Worker did not exit without starting the player.'
        }
        $stdout.GetAwaiter().GetResult() | Out-Null
        $detail = $stderr.GetAwaiter().GetResult()
        if ($process.ExitCode -ne $ExpectedCode) {
            throw "Worker exit $($process.ExitCode), expected ${ExpectedCode}: $detail"
        }
    } finally { $process.Dispose() }
}
try {
    Invoke-Worker '--analysis-worker' 1
    Invoke-Worker "--analysis-worker `"$root`" extra" 1
    Invoke-Worker "--analysis-worker `"$root`"" 2
    [IO.File]::WriteAllText((Join-Path $root 'request.json'), (@{
        operation = 'transcribe_speech'; model = (Join-Path $root 'missing.bin')
        alignment_preset = 0; task = 0; source_language = 'en'; sample_count = 1
    } | ConvertTo-Json))
    [IO.File]::WriteAllBytes((Join-Path $root 'audio.f32'), [byte[]]@(0, 0, 0, 0))
    Invoke-Worker "--analysis-worker `"$root`"" 4
    $result = Get-Content -LiteralPath (Join-Path $root 'result.json') -Raw | ConvertFrom-Json
    if ($result.operation -ne 'transcribe_speech' -or -not $result.detail -or $result.segments.Count -ne 0) {
        throw 'Worker did not return a structured model-load failure.'
    }
    Write-Host 'Isolated worker dispatch, Unicode paths and error protocol passed.'
} finally {
    # Only the files produced by this fixture, with no recursive cleanup.
    foreach ($name in @('request.json', 'audio.f32', 'progress.json', 'result.json')) {
        $path = Join-Path $root $name
        if ([IO.File]::Exists($path)) { [IO.File]::Delete($path) }
    }
    [IO.Directory]::Delete($root)
}
