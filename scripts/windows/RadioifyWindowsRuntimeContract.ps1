. (Join-Path $PSScriptRoot "RadioifyWindowsPackageFiles.ps1")

function Get-RadioifyWindowsMlRuntimeContract {
    [CmdletBinding()]
    param()

    return [pscustomobject]@{
        ProductionRuntimeFiles = [string[]]@(
            "onnxruntime.dll"
            "Microsoft.Windows.AI.MachineLearning.dll"
        )
        DiagnosticOnlyRuntimeFiles = [string[]]@(
            "DirectML.dll"
        )
    }
}
function Copy-RadioifyFileIfChanged {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination,
        [string]$SourceDigest
    )
    $sourceFile = Get-Item -LiteralPath $Source -Force -ErrorAction Stop
    $targetFile = Get-Item -LiteralPath $Destination -Force -ErrorAction SilentlyContinue
    if (-not $SourceDigest) { $SourceDigest = Get-RadioifyFileDigest -Path $Source }
    if ($targetFile -and $sourceFile.Length -eq $targetFile.Length -and
        (Get-RadioifyFileDigest -Path $Destination) -eq $SourceDigest) {
        return $false
    }
    [System.IO.Directory]::CreateDirectory((Split-Path -Parent $Destination)) | Out-Null
    if ($sourceFile.Length -ge 256MB) {
        Write-Host ("Copying {0} ({1:N2} GiB)..." -f $sourceFile.Name, ($sourceFile.Length / 1GB))
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
    if ((Get-RadioifyFileDigest -Path $Destination) -ne $SourceDigest) {
        throw "The packaging input changed while it was being copied: '$Source'. Retry after the build finishes."
    }
    return $true
}

function Remove-RadioifyRetiredPayloadFiles {
    param([Parameter(Mandatory = $true)][string]$OutputRoot)
    $root = (Get-Item -LiteralPath $OutputRoot -ErrorAction Stop).FullName.TrimEnd('\')
    if ((Get-Item -LiteralPath $root).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "Refusing to clean a redirected build directory: $root"
    }
    foreach ($relative in @("radioify_analysis_worker.exe", "models\audio_separation\bandit-v2-multi-mask-core-fp16.onnx")) {
        $path = [IO.Path]::GetFullPath((Join-Path $root $relative))
        if (-not $path.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Retired payload escaped its build directory."
        }
        $file = Get-Item -LiteralPath $path -ErrorAction SilentlyContinue
        if (-not $file) { continue }
        for ($item = $file; $item.FullName -ne $root; $item = Get-Item -LiteralPath (Split-Path -Parent $item.FullName)) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Refusing to remove a redirected payload: $path"
            }
        }
        if ($file.PSIsContainer) { throw "Expected a generated file at $path" }
        $file.Delete()
        Write-Host "Removed retired build payload: $path"
    }
}

function Assert-RadioifyTranscriptRuntime {
    param(
        [Parameter(Mandatory = $true)][string]$Source
    )
    foreach ($relative in @("runtime.json", "worker.py", "python.exe", "python313.dll", "msvcp140.dll",
                           "python313._pth", "Lib\site-packages\torch\__init__.py",
                           "models\Qwen3-ASR-1.7B-hf\model.safetensors", "models\large-v3.pt")) {
        if (-not (Test-Path -LiteralPath (Join-Path $Source $relative) -PathType Leaf)) {
            throw "Qwen3-ASR transcript runtime is incomplete: '$Source\$relative'."
        }
    }
}

function Copy-RadioifyTranscriptRuntime {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    Assert-RadioifyTranscriptRuntime -Source $Source
    & robocopy.exe $Source $Destination /E /R:2 /W:1 /NFL /NDL /NJH /NJS /NP /XD __pycache__ .cache /XF *.pyc | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Could not package Qwen3-ASR transcript runtime." }
}

function Get-RadioifyTranscriptBootstrapFiles {
    return @("worker.py", "setup_runtime.ps1", "setup_runtime.py", "runtime-manifest.json",
             "requirements.lock", "verify_runtime.py", "NOTICE")
}

function Copy-RadioifyTranscriptBootstrap {
    param([string]$Source, [string]$Destination)
    foreach ($name in Get-RadioifyTranscriptBootstrapFiles) {
        [void](Copy-RadioifyFileIfChanged -Source (Join-Path $Source $name) -Destination (Join-Path $Destination $name))
    }
    if (-not (Test-Path -LiteralPath (Join-Path $Source "native\msvcp140.dll"))) {
        throw "The speech bootstrap C++ runtime is missing."
    }
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $Source "native") -Filter '*.dll') {
        [void](Copy-RadioifyFileIfChanged -Source $file.FullName -Destination (Join-Path $Destination "native\$($file.Name)"))
    }
}
