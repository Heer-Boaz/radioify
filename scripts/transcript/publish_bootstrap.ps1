param([Parameter(Mandatory = $true)][string]$Destination)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "..\windows\RadioifyWindowsRuntimeContract.ps1")
[IO.Directory]::CreateDirectory($Destination) | Out-Null
foreach ($name in Get-RadioifyTranscriptBootstrapFiles) {
    [void](Copy-RadioifyFileIfChanged -Source (Join-Path $PSScriptRoot $name) -Destination (Join-Path $Destination $name))
}
# These small redistributable DLLs let downloaded Python/CUDA run without a
# system Python installation or a separate machine-wide VC runtime installer.
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw "Visual Studio C++ redistributable files could not be located." }
$redist = Get-ChildItem -LiteralPath (Join-Path $vsRoot "VC\Redist\MSVC") -Directory |
    Where-Object Name -Match '^\d+\.\d+\.\d+$' | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$crt = Get-ChildItem -LiteralPath (Join-Path $redist.FullName "x64") -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
if (-not $crt) { throw "The x64 C++ redistributable is missing." }
foreach ($file in Get-ChildItem -LiteralPath $crt.FullName -Filter '*.dll') {
    [void](Copy-RadioifyFileIfChanged -Source $file.FullName -Destination (Join-Path $Destination "native\$($file.Name)"))
}
Write-Host "Speech bootstrap ready; Python/CUDA and speech models download on first use."
