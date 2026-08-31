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
