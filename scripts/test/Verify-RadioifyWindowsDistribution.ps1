[CmdletBinding()]
param([switch]$LargeFile)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
. (Join-Path $repoRoot 'scripts\windows\RadioifyWindowsReleasePackaging.ps1')
function Assert-True { param([bool]$Condition, [string]$Message) if (-not $Condition) { throw $Message } }
function Write-Fixture {
    param([string]$Path, [string]$Text = 'fixture')
    [IO.Directory]::CreateDirectory((Split-Path -Parent $Path)) | Out-Null
    [IO.File]::WriteAllText($Path, $Text)
}
$tempParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$root = Join-Path $tempParent ('radioify-distribution-test-' + [guid]::NewGuid().ToString('N'))
$fixtureRepo = Join-Path $root 'repo'
$outputRoot = Join-Path $root 'output'
$executable = Join-Path $fixtureRepo 'dist\radioify.exe'
$script:failIntegration = $false
function Copy-RadioifyWindowsExplorerIntegrationBundle {
    param($RepoRoot, $StageDir)
    if ($script:failIntegration) { throw 'Fixture integration failure' }
    Write-Fixture (Join-Path $StageDir 'Radioify.Fixture.msix')
}
$script:RadioifyWindowsBundleFiles = @(
    @{ Source = 'install.ps1'; Destination = 'install_radioify.ps1' },
    @{ Source = 'hidden.txt'; Destination = 'hidden.txt' }
)
try {
    foreach ($relative in @('radioify.exe', 'llama-cpp-LICENSE.txt',
            'models\audio_separation\BandIt-v2-MODEL-CARD', 'onnxruntime.dll',
            'Microsoft.Windows.AI.MachineLearning.dll', 'transcript\native\msvcp140.dll')) {
        Write-Fixture (Join-Path $fixtureRepo "dist\$relative")
    }
    foreach ($file in Get-RadioifyTranscriptBootstrapFiles) {
        Write-Fixture (Join-Path $fixtureRepo "dist\transcript\$file")
    }
    Write-Fixture (Join-Path $fixtureRepo 'install.ps1')
    Write-Fixture (Join-Path $fixtureRepo 'hidden.txt') 'hidden payload'
    $hidden = Get-Item -LiteralPath (Join-Path $fixtureRepo 'hidden.txt')
    $hidden.Attributes = $hidden.Attributes -bor [IO.FileAttributes]::Hidden
    $arguments = @{ RepoRoot = $fixtureRepo; ExecutablePath = $executable; OutputRoot = $outputRoot }
    $first = New-RadioifyWindowsDistributionBundle @arguments -Version '1.0.0'
    $firstDigest = Get-RadioifyFileDigest -Path $first.ZipPath
    $archive = [IO.Compression.ZipFile]::OpenRead($first.ZipPath)
    try {
        Assert-True ($null -ne $archive.GetEntry('Radioify-Windows-x64/hidden.txt')) 'Hidden payload was omitted from the ZIP.'
        Assert-True ($null -ne $archive.GetEntry('Radioify-Windows-x64/RadioifyPackage.json')) 'ZIP root directory changed.'
    } finally { $archive.Dispose() }

    Write-Fixture $executable 'new application'
    $script:failIntegration = $true
    $failed = $false
    try { New-RadioifyWindowsDistributionBundle @arguments -Version '2.0.0' | Out-Null }
    catch { $failed = $_.Exception.Message -match 'Fixture integration failure' }
    Assert-True $failed 'The export swallowed an input failure.'
    Assert-True ((Get-RadioifyFileDigest $first.ZipPath) -eq $firstDigest) 'Input failure replaced the previous ZIP.'
    Assert-True ((Get-Content (Join-Path $first.StageDir 'radioify.exe') -Raw) -eq 'fixture') 'Input failure replaced the previous bundle.'
    Assert-True (-not (Test-Path (Join-Path $outputRoot '.Radioify-Windows-x64.building'))) 'Failed export left staging behind.'
    $script:failIntegration = $false

    # A real sharing violation at the final publication boundary must restore
    # both the old extracted bundle and the old ZIP.
    $held = [IO.File]::Open($first.ZipPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $failed = $false
        try { New-RadioifyWindowsDistributionBundle @arguments -Version '2.0.0' | Out-Null }
        catch { $failed = $true }
        Assert-True $failed 'An exclusively held archive did not prevent replacement.'
    } finally { $held.Dispose() }
    Assert-True ((Get-RadioifyFileDigest $first.ZipPath) -eq $firstDigest) 'Publication failure damaged the previous ZIP.'
    Assert-True ((Get-Content (Join-Path $first.StageDir 'radioify.exe') -Raw) -eq 'fixture') 'Publication failure did not roll back the bundle.'
    Assert-True (-not (Test-Path "$($first.ZipPath).building.zip")) 'Failed publication left a partial ZIP.'

    $second = New-RadioifyWindowsDistributionBundle @arguments -Version '2.0.0'
    Assert-True ((Get-Content (Join-Path $second.StageDir 'radioify.exe') -Raw) -eq 'new application') 'Successful export retained old payload.'
    Assert-True ((Get-RadioifyFileDigest $second.ZipPath) -ne $firstDigest) 'Successful export did not publish the ZIP.'

    $backup = Join-Path $outputRoot '.Radioify-Windows-x64.previous'
    Get-RadioifyOwnedBundleItems -Path $second.StageDir -ExpectedParent $outputRoot | Out-Null
    [IO.Directory]::Move($second.StageDir, $backup)
    $script:failIntegration = $true
    try { New-RadioifyWindowsDistributionBundle @arguments -Version '3.0.0' | Out-Null } catch {}
    Assert-True (Test-Path (Join-Path $second.StageDir 'radioify.exe')) 'An interrupted directory swap was not recovered.'

    if ($LargeFile) {
        # Exercise the original 2 GB failure boundary without allocating GBs of
        # temporary disk space. Only the final four bytes occupy a sparse extent.
        if (-not ('RadioifySparseFixture' -as [type])) {
            Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class RadioifySparseFixture {
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool DeviceIoControl(SafeFileHandle file, uint code,
        IntPtr input, uint inputSize, IntPtr output, uint outputSize,
        out uint returned, IntPtr overlapped);
    [DllImport("kernel32.dll", EntryPoint = "GetCompressedFileSizeW", ExactSpelling = true,
        CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetCompressedFileSize(string path, out uint high);
    public static void MakeSparse(FileStream stream) {
        uint returned;
        if (!DeviceIoControl(stream.SafeFileHandle, 0x900C4, IntPtr.Zero, 0,
            IntPtr.Zero, 0, out returned, IntPtr.Zero))
            throw new Win32Exception(Marshal.GetLastWin32Error());
    }
    public static ulong AllocatedBytes(string path) {
        uint high;
        uint low = GetCompressedFileSize(path, out high);
        if (low == uint.MaxValue && Marshal.GetLastWin32Error() != 0)
            throw new Win32Exception(Marshal.GetLastWin32Error());
        return ((ulong)high << 32) | low;
    }
}
'@
        }
        $largeStage = Join-Path $root 'large-bundle'
        Write-Fixture (Join-Path $largeStage 'RadioifyPackage.json') ((New-RadioifyWindowsPackageManifest -Version 'fixture' -ExecutableName 'radioify.exe') | ConvertTo-Json)
        $largePath = Join-Path $largeStage 'large.dat'
        $stream = [IO.File]::Open($largePath, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        try {
            [RadioifySparseFixture]::MakeSparse($stream)
            $stream.SetLength([long]2GB + 37)
            $stream.Position = $stream.Length - 4
            $stream.Write([byte[]]@(70, 73, 78, 33), 0, 4)
        } finally { $stream.Dispose() }
        $allocated = [RadioifySparseFixture]::AllocatedBytes($largePath)
        Assert-True ($allocated -lt 16MB) 'Large-file fixture allocated too much disk space.'
        Write-Host "Verifying a file larger than 2 GB; physical fixture allocation: $allocated bytes."
        $largeZip = Join-Path $root 'large.zip'
        New-RadioifyDistributionZip -StageDir $largeStage -ArchivePath $largeZip -RootName 'Radioify-Fixture'
        $archive = [IO.Compression.ZipFile]::OpenRead($largeZip)
        try {
            Assert-True ($archive.GetEntry('Radioify-Fixture/large.dat').Length -gt [int]::MaxValue) 'Large ZIP entry was truncated.'
        } finally { $archive.Dispose() }
        Write-Host 'Streaming ZIP export above 2 GB passed content verification.'
    }

    $unowned = Join-Path $outputRoot '.Radioify-Windows-x64.building'
    Write-Fixture (Join-Path $unowned 'notes.txt') 'preserve me'
    $rejected = $false
    try { New-RadioifyWindowsDistributionBundle @arguments -Version '3.0.0' | Out-Null }
    catch { $rejected = $true }
    Assert-True ($rejected -and (Get-Content (Join-Path $unowned 'notes.txt') -Raw) -eq 'preserve me') 'Unowned files were removed.'
    Write-Host 'Distribution ZIP verification, failure rollback, recovery and cleanup passed.'
} finally {
    if (-not $root.StartsWith($tempParent + '\radioify-distribution-test-', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Unsafe fixture cleanup path.'
    }
    $items = @(Get-ChildItem -LiteralPath $root -Recurse -Force)
    foreach ($item in $items) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Redirected fixture cleanup path.' }
    }
    foreach ($item in $items | Where-Object { -not $_.PSIsContainer }) { $item.Delete() }
    foreach ($item in $items | Where-Object { $_.PSIsContainer } | Sort-Object { $_.FullName.Length } -Descending) { $item.Delete() }
    (Get-Item -LiteralPath $root).Delete()
}
