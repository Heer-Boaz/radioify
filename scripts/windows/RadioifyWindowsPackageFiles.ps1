function Get-RadioifyFileDigest {
    param([Parameter(Mandatory = $true)][string]$Path)
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
        finally { $sha.Dispose() }
    } finally { $stream.Dispose() }
}

function Enter-RadioifyPackageLock {
    param(
        [Parameter(Mandatory = $true)][string]$Directory,
        [Parameter(Mandatory = $true)][string]$Name,
        [int]$TimeoutSeconds = 600
    )
    $path = Join-Path $Directory $Name
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $nextReport = 0
    while ($true) {
        try {
            # The OS releases this lock if its owner crashes. The empty lock
            # file stays in place so opening/unlinking it cannot introduce a race.
            return [IO.File]::Open($path, [IO.FileMode]::OpenOrCreate,
                [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        } catch [IO.IOException] {
            if (($_.Exception.HResult -band 0xffff) -notin @(32, 33)) { throw }
            if ($timer.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
                throw "Timed out waiting for the packaging run using '$Directory'."
            }
            if ($timer.Elapsed.TotalSeconds -ge $nextReport) {
                Write-Host "Waiting for another packaging run to finish: $Directory"
                $nextReport = $timer.Elapsed.TotalSeconds + 10
            }
            Start-Sleep -Milliseconds 100
        }
    }
}

function Publish-RadioifyPackageFile {
    param(
        [Parameter(Mandatory = $true)][string]$PendingPath,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    $pending = [IO.Path]::GetFullPath($PendingPath)
    $target = [IO.Path]::GetFullPath($Destination)
    if (-not [IO.Path]::GetDirectoryName($pending).Equals(
            [IO.Path]::GetDirectoryName($target), [StringComparison]::OrdinalIgnoreCase)) {
        throw "Package publication must stay within one output directory."
    }
    if ([IO.File]::Exists($target)) { [IO.File]::Replace($pending, $target, [NullString]::Value) }
    else { [IO.File]::Move($pending, $target) }
}

function Write-RadioifyPackageState {
    param([string]$Path, [Parameter(Mandatory = $true)]$State)
    $pending = "$Path.writing"
    try {
        [IO.File]::WriteAllText($pending, ($State | ConvertTo-Json -Depth 8),
            [Text.UTF8Encoding]::new($false))
        Publish-RadioifyPackageFile -PendingPath $pending -Destination $Path
    } finally {
        if ([IO.File]::Exists($pending)) { [IO.File]::Delete($pending) }
    }
}

function Get-RadioifyOwnedBundleItems {
    param([string]$Path, [string]$ExpectedParent)
    $root = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    $parent = [IO.Path]::GetFullPath($ExpectedParent).TrimEnd('\')
    if (-not $root.PSIsContainer -or
        -not $root.Parent.FullName.TrimEnd('\').Equals($parent, [StringComparison]::OrdinalIgnoreCase) -or
        ($root.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Bundle directory is outside its output root or is redirected: '$Path'."
    }
    $items = [Collections.Generic.List[IO.FileSystemInfo]]::new()
    $pending = [Collections.Generic.Stack[IO.DirectoryInfo]]::new()
    $items.Add($root)
    $pending.Push($root)
    while ($pending.Count) {
        foreach ($item in $pending.Pop().GetFileSystemInfos()) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Bundle contains a redirected path: '$($item.FullName)'."
            }
            $items.Add($item)
            if ($item -is [IO.DirectoryInfo]) { $pending.Push($item) }
        }
    }
    if ($items.Count -gt 1) {
        $manifest = Get-Content -LiteralPath (Join-Path $root.FullName 'RadioifyPackage.json') -Raw -ErrorAction Stop | ConvertFrom-Json
        if ($manifest.PackageFormatVersion -ne 1 -or $manifest.DisplayName -ne 'Radioify' -or
            $manifest.ExecutableName -ne 'radioify.exe') {
            throw "The output directory is not an owned Radioify bundle: '$Path'."
        }
    }
    return $items
}

function Remove-RadioifyOwnedBundleDirectory {
    param([string]$Path, [string]$ExpectedParent)
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $items = @(Get-RadioifyOwnedBundleItems -Path $Path -ExpectedParent $ExpectedParent)
    foreach ($file in $items | Where-Object { $_ -is [IO.FileInfo] }) {
        $file.IsReadOnly = $false
        $file.Delete()
    }
    foreach ($directory in $items | Where-Object { $_ -is [IO.DirectoryInfo] } |
            Sort-Object { $_.FullName.Length } -Descending) {
        $directory.Delete()
    }
}

function New-RadioifyDistributionZip {
    param([string]$StageDir, [string]$ArchivePath, [string]$RootName)
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $stage = (Get-Item -LiteralPath $StageDir).FullName.TrimEnd('\')
    $files = @(Get-RadioifyOwnedBundleItems -Path $stage -ExpectedParent (Split-Path -Parent $stage) |
        Where-Object { $_ -is [IO.FileInfo] } | Sort-Object FullName)
    $expected = @{}
    $output = [IO.File]::Open($ArchivePath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        # Create mode writes directly to disk and supports ZIP64. In particular,
        # no archive entry is first collected in a size-limited MemoryStream.
        $archive = [IO.Compression.ZipArchive]::new($output, [IO.Compression.ZipArchiveMode]::Create, $true)
        try {
            foreach ($file in $files) {
                $name = $RootName + '/' + $file.FullName.Substring($stage.Length + 1).Replace('\', '/')
                $expected[$name] = @{ Length = $file.Length; Digest = Get-RadioifyFileDigest -Path $file.FullName }
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                    $archive, $file.FullName, $name, [IO.Compression.CompressionLevel]::Optimal)
            }
        } finally { $archive.Dispose() }
    } finally { $output.Dispose() }

    Write-Host "Verifying distribution ZIP contents..."
    $archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        if ($archive.Entries.Count -ne $expected.Count) { throw 'Distribution ZIP has an unexpected file count.' }
        foreach ($entry in $archive.Entries) {
            if (-not $expected.ContainsKey($entry.FullName) -or $entry.Length -ne $expected[$entry.FullName].Length) {
                throw "Distribution ZIP has an unexpected entry: '$($entry.FullName)'."
            }
            $stream = $entry.Open()
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $digest = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
            finally { $sha.Dispose(); $stream.Dispose() }
            if ($digest -ne $expected[$entry.FullName].Digest) {
                throw "Distribution ZIP failed content verification: '$($entry.FullName)'."
            }
            $expected.Remove($entry.FullName)
        }
    } finally { $archive.Dispose() }
}
