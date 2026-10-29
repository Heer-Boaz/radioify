param(
    [Parameter(Mandatory = $true)][string]$Workspace,
    [Parameter(Mandatory = $true)][string]$CacheRoot,
    [ValidateSet("quality", "whisper")][string]$Component = "quality",
    [string]$RuntimePath
)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Write-SetupJson([string]$Name, $Value) {
    $path = Join-Path $Workspace $Name
    $temporary = "$path.writing"
    [IO.File]::WriteAllText($temporary, ($Value | ConvertTo-Json -Compress), [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporary -Destination $path -Force
}
function Report-Setup([string]$Phase) {
    Write-SetupJson "setup-progress.json" @{ phase = $Phase }
}
function Get-SetupHash([string]$Path) {
    $hash = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try { return [BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $hash.Dispose() }
}

$mutex = $null
$owned = $false
try {
    $manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot "runtime-manifest.json") -Raw | ConvertFrom-Json
    $requirementsHash = Get-SetupHash (Join-Path $PSScriptRoot "requirements.lock")
    [IO.Directory]::CreateDirectory($CacheRoot) | Out-Null
    $locationPath = Join-Path $CacheRoot ("location-" + $requirementsHash.Substring(0, 12).ToLowerInvariant() + ".json")
    if (-not $RuntimePath -and (Test-Path -LiteralPath $locationPath)) {
        try {
            $location = Get-Content -LiteralPath $locationPath -Raw | ConvertFrom-Json
            if (Test-Path -LiteralPath $location.runtime -PathType Container) { $RuntimePath = $location.runtime }
        } catch { }
    }
    if (-not $RuntimePath) { $RuntimePath = Join-Path $CacheRoot ("v1-" + $requirementsHash.Substring(0, 12).ToLowerInvariant()) }
    $RuntimePath = [IO.Path]::GetFullPath($RuntimePath)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $lockId = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($RuntimePath.ToLowerInvariant()))).Replace('-', '') }
    finally { $sha.Dispose() }
    $mutex = [Threading.Mutex]::new($false, "Local\Radioify.TranscriptSetup.$lockId")
    Report-Setup "Waiting for speech runtime setup"
    while (-not $owned) {
        try { $owned = $mutex.WaitOne(250) }
        catch [Threading.AbandonedMutexException] { $owned = $true }
    }
    [IO.Directory]::CreateDirectory($RuntimePath) | Out-Null
    $python = Join-Path $RuntimePath "python.exe"
    $pythonMarker = Join-Path $RuntimePath "python-install.json"
    $ready = $false
    if (Test-Path -LiteralPath $pythonMarker) {
        try { $ready = (Get-Content -LiteralPath $pythonMarker -Raw | ConvertFrom-Json).sha256 -eq $manifest.python.sha256 }
        catch { $ready = $false }
    } elseif (Test-Path -LiteralPath (Join-Path $RuntimePath "runtime.json")) {
        # An explicitly selected existing development cache needs no second copy.
        try { $ready = (Get-Content -LiteralPath (Join-Path $RuntimePath "runtime.json") -Raw | ConvertFrom-Json).python -eq "3.13.15" }
        catch { $ready = $false }
    }
    foreach ($name in @("python.exe", "python313.dll", "python313.zip")) {
        $ready = $ready -and (Test-Path -LiteralPath (Join-Path $RuntimePath $name) -PathType Leaf)
    }
    if (-not $ready) {
        Report-Setup "Downloading speech runtime (Python, 11 MB; first use)"
        $archive = Join-Path $RuntimePath "python-embed.zip.download"
        Invoke-WebRequest -UseBasicParsing -Uri $manifest.python.url -OutFile $archive
        if ((Get-Item -LiteralPath $archive).Length -ne $manifest.python.bytes -or
            (Get-SetupHash $archive) -ne $manifest.python.sha256) {
            throw "Speech runtime Python download failed checksum verification."
        }
        Report-Setup "Unpacking speech runtime"
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zip = [IO.Compression.ZipFile]::OpenRead($archive)
        try {
            foreach ($entry in $zip.Entries) {
                if (-not $entry.Name) { continue }
                $destination = [IO.Path]::GetFullPath((Join-Path $RuntimePath $entry.FullName))
                if (-not $destination.StartsWith($RuntimePath.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
                    throw "Python archive entry escapes the runtime directory."
                }
                [IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destination, $true)
            }
        } finally { $zip.Dispose() }
        [IO.File]::WriteAllText($pythonMarker, (@{sha256=$manifest.python.sha256} | ConvertTo-Json))
        Remove-Item -LiteralPath $archive -Force
    }
    [IO.File]::WriteAllLines((Join-Path $RuntimePath "python313._pth"), @("python313.zip", ".", "Lib\site-packages", "import site"))
    foreach ($dll in Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot "native") -Filter '*.dll') {
        $destination = Join-Path $RuntimePath $dll.Name
        $existing = Get-Item -LiteralPath $destination -ErrorAction SilentlyContinue
        if (-not $existing -or $existing.Length -ne $dll.Length -or $existing.LastWriteTimeUtc -ne $dll.LastWriteTimeUtc) {
            Copy-Item -LiteralPath $dll.FullName -Destination $destination -Force
        }
    }
    & $python -I -B (Join-Path $PSScriptRoot "setup_runtime.py") $Workspace $RuntimePath $Component
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    # Preserve a deliberately selected existing cache across app upgrades and
    # packaged/portable launches, without copying its multi-GB payload.
    [IO.File]::WriteAllText($locationPath, (@{runtime=$RuntimePath} | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
} catch {
    Write-SetupJson "setup-result.json" @{ version=1; status="error"; error=$_.Exception.Message }
    exit 1
} finally {
    if ($owned) { $mutex.ReleaseMutex() }
    if ($mutex) { $mutex.Dispose() }
}
