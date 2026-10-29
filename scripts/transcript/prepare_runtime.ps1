[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Destination,
    [string]$SitePackagesSource,
    [string]$ModelsSource,
    [string]$BuildPython = "python"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$cache = Join-Path $repoRoot "models\transcript-runtime"
$downloads = Join-Path $repoRoot "models\transcript-runtime-downloads"
$lockFile = Join-Path $PSScriptRoot "requirements.lock"
$lockHash = (Get-FileHash -LiteralPath $lockFile -Algorithm SHA256).Hash
. (Join-Path $repoRoot "scripts\windows\RadioifyWindowsRuntimeContract.ps1")

function Copy-RuntimeTree([string]$Source, [string]$Target) {
    & robocopy.exe $Source $Target /E /R:2 /W:1 /NFL /NDL /NJH /NJS /NP /XD __pycache__ .cache /XF *.pyc | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Could not copy transcript runtime from '$Source'." }
}

$markerPath = Join-Path $cache "runtime.json"
$ready = $false
if (Test-Path -LiteralPath $markerPath) {
    $marker = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
    $ready = $marker.version -eq 1 -and $marker.requirements_sha256 -eq $lockHash
    if (-not $ready) {
        throw "Transcript runtime cache is from another dependency set. Move '$cache' aside and provision again."
    }
}
if (-not $ready) {
    New-Item -ItemType Directory -Force -Path $cache, $downloads | Out-Null
    $archive = Join-Path $downloads "python-3.13.15-embed-amd64.zip"
    if (-not (Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest -Uri "https://www.python.org/ftp/python/3.13.15/python-3.13.15-embed-amd64.zip" -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne "D1F04D990AEE1253D8569E8E5104E30FA9F5FA830899F14843448872D936A2CF") {
        throw "Embedded Python checksum mismatch."
    }
    Expand-Archive -LiteralPath $archive -DestinationPath $cache -Force
    # Isolated and relocatable: no system Python, user site or PYTHONPATH.
    @("python313.zip", ".", "Lib\site-packages", "import site") |
        Set-Content -LiteralPath (Join-Path $cache "python313._pth") -Encoding ascii
    $packages = Join-Path $cache "Lib\site-packages"
    if ($SitePackagesSource) {
        Copy-RuntimeTree -Source $SitePackagesSource -Target $packages
    } else {
        & $BuildPython -c "import sys; assert sys.version_info[:2] == (3, 13), 'BuildPython must be Python 3.13 to match the embedded runtime'"
        if ($LASTEXITCODE -ne 0) { throw "Use -BuildPython with a Python 3.13 interpreter containing pip." }
        & $BuildPython -m pip install --target $packages --no-compile `
            --extra-index-url https://download.pytorch.org/whl/cu128 -r $lockFile
        if ($LASTEXITCODE -ne 0) { throw "Could not provision pinned transcript dependencies." }
    }
    $python = Join-Path $cache "python.exe"
    & $python -I -B (Join-Path $PSScriptRoot "verify_runtime.py") $lockFile
    if ($LASTEXITCODE -ne 0) { throw "Transcript Python dependencies failed validation." }
    $models = Join-Path $cache "models"
    New-Item -ItemType Directory -Force -Path $models | Out-Null
    $qwen = Join-Path $models "Qwen3-ASR-1.7B-hf"
    if ($ModelsSource) {
        Copy-RuntimeTree -Source (Join-Path $ModelsSource "Qwen3-ASR-1.7B-hf") -Target $qwen
        Copy-Item -LiteralPath (Join-Path $ModelsSource "large-v3.pt") -Destination $models -Force
    } else {
        & hf download Qwen/Qwen3-ASR-1.7B-hf --revision bcd2b5b7f32b480ab5790554cfa8347f246a14f3 --local-dir $qwen
        if ($LASTEXITCODE -ne 0) { throw "Could not download pinned Qwen3-ASR model." }
        Invoke-WebRequest -Uri "https://openaipublic.azureedge.net/main/whisper/models/e5b1a55b89c1367dacf97e3e19bfd829a01529dbfdeefa8caeb59b3f1b81dadb/large-v3.pt" `
            -OutFile (Join-Path $models "large-v3.pt")
    }
    if ((Get-FileHash -LiteralPath (Join-Path $models "large-v3.pt") -Algorithm SHA256).Hash -ne "e5b1a55b89c1367dacf97e3e19bfd829a01529dbfdeefa8caeb59b3f1b81dadb") {
        throw "Whisper large-v3 checksum mismatch."
    }
    if ((Get-FileHash -LiteralPath (Join-Path $qwen "model.safetensors") -Algorithm SHA256).Hash -ne "2DB53C7D81BD9B8CBC6A074E89BE2C968A0D373FB4EE68BB1B1E14F7042DFEE1") {
        throw "Qwen3-ASR 1.7B checksum mismatch."
    }
    @{
        version = 1
        requirements_sha256 = $lockHash
        python = "3.13.15"
        recognition = "Qwen/Qwen3-ASR-1.7B-hf"
        recognition_revision = "bcd2b5b7f32b480ab5790554cfa8347f246a14f3"
        alignment = "Whisper large-v3 / stable-ts 2.19.1"
    } | ConvertTo-Json | Set-Content -LiteralPath $markerPath -Encoding utf8
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "worker.py") -Destination $cache -Force
if (-not (Test-Path -LiteralPath (Join-Path $cache "msvcp140.dll"))) {
    # PyTorch uses the shared C++ runtime even though Radioify itself is static.
    # Bundle the redistributable DLLs so the target PC needs no VC installer.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsRoot) { throw "Visual Studio C++ redistributable files could not be located." }
    $redistVersion = Get-ChildItem -LiteralPath (Join-Path $vsRoot "VC\Redist\MSVC") -Directory |
        Where-Object Name -Match '^\d+\.\d+\.\d+$' |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if (-not $redistVersion) { throw "Visual Studio C++ redistributable files are missing." }
    $crt = Get-ChildItem -LiteralPath (Join-Path $redistVersion.FullName "x64") -Directory -Filter 'Microsoft.VC*.CRT' |
        Select-Object -First 1
    if (-not $crt) { throw "The x64 C++ runtime is missing." }
    Get-ChildItem -LiteralPath $crt.FullName -Filter '*.dll' |
        Copy-Item -Destination $cache -Force
}
Copy-Item -LiteralPath $lockFile -Destination $cache -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "NOTICE") -Destination $cache -Force
Copy-RadioifyTranscriptRuntime -Source $cache -Destination $Destination
Write-Host "Qwen3-ASR 1.7B + Whisper large-v3 runtime ready: $Destination"
