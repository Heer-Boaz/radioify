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
function Copy-RadioifyTranscriptRuntime {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    foreach ($relative in @("runtime.json", "worker.py", "python.exe", "python313.dll", "msvcp140.dll",
                           "python313._pth", "Lib\site-packages\torch\__init__.py",
                           "models\Qwen3-ASR-1.7B-hf\model.safetensors", "models\large-v3.pt")) {
        if (-not (Test-Path -LiteralPath (Join-Path $Source $relative) -PathType Leaf)) {
            throw "Qwen3-ASR transcript runtime is incomplete: '$Source\$relative'."
        }
    }
    & robocopy.exe $Source $Destination /E /R:2 /W:1 /NFL /NDL /NJH /NJS /NP /XD __pycache__ .cache /XF *.pyc | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Could not package Qwen3-ASR transcript runtime." }
}
