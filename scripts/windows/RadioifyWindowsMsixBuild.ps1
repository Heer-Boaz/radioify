Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "RadioifyWindowsMsixCommon.ps1")
. (Join-Path $PSScriptRoot "RadioifyWindowsRuntimeContract.ps1")

function Resolve-RadioifyWindowsSdkExecutable {
    param([Parameter(Mandatory = $true)][string]$ToolName)

    $command = Get-Command $ToolName -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command -and $command.Source) {
        return $command.Source
    }

    $searchRoots = @()
    if ($env:ProgramFiles) {
        $searchRoots += (Join-Path $env:ProgramFiles "Windows Kits\10\bin")
    }
    $programFilesX86 = [Environment]::GetEnvironmentVariable("ProgramFiles(x86)")
    if ($programFilesX86) {
        $searchRoots += (Join-Path $programFilesX86 "Windows Kits\10\bin")
    }

    foreach ($root in ($searchRoots | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $versionDirs = Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending
        foreach ($versionDir in $versionDirs) {
            foreach ($arch in @("x64", "x86")) {
                $candidate = Join-Path $versionDir.FullName "$arch\$ToolName"
                if (Test-Path -LiteralPath $candidate) {
                    return $candidate
                }
            }
        }
    }

    throw "Windows SDK tool '$ToolName' was not found."
}

function ConvertTo-RadioifyNativeCommandLineArgument {
    param([AllowEmptyString()][string]$Argument)

    if ($null -eq $Argument) {
        return '""'
    }
    if ($Argument.Length -gt 0 -and $Argument -notmatch '[\s"]') {
        return $Argument
    }

    $escaped = $Argument -replace '(\\*)"', '$1$1\"'
    $escaped = $escaped -replace '(\\+)$', '$1$1'
    return '"' + $escaped + '"'
}

function Join-RadioifyNativeCommandLineArguments {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    return (($Arguments | ForEach-Object {
        ConvertTo-RadioifyNativeCommandLineArgument -Argument $_
    }) -join " ")
}

function Invoke-RadioifyNativeTool {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string]$Activity = "Running native tool",
        [string]$LogPath,
        [ValidateRange(1, 60)][int]$ProgressIntervalSeconds = 10
    )

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = Join-RadioifyNativeCommandLineArguments -Arguments $Arguments
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    Write-Host "$Activity..."
    $elapsed = [System.Diagnostics.Stopwatch]::StartNew()
    $process = [System.Diagnostics.Process]::Start($startInfo)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $nextProgress = $ProgressIntervalSeconds
        while (-not $process.WaitForExit(250)) {
            if ($elapsed.Elapsed.TotalSeconds -ge $nextProgress) {
                Write-Host ("{0}: {1:N0}s elapsed" -f $Activity, $elapsed.Elapsed.TotalSeconds)
                $nextProgress += $ProgressIntervalSeconds
            }
        }

        $stdoutText = $stdout.Result
        $stderrText = $stderr.Result
        if ($LogPath) {
            [System.IO.Directory]::CreateDirectory((Split-Path -Parent $LogPath)) | Out-Null
            [System.IO.File]::WriteAllText($LogPath, $stdoutText + [Environment]::NewLine + $stderrText)
            Write-Host "Tool output: $LogPath"
        } else {
            if (-not [string]::IsNullOrWhiteSpace($stdoutText)) { Write-Host $stdoutText.TrimEnd() }
            if (-not [string]::IsNullOrWhiteSpace($stderrText)) { Write-Host $stderrText.TrimEnd() }
        }

        if ($process.ExitCode -ne 0) {
            if ($LogPath) {
                Get-Content -LiteralPath $LogPath -Tail 30 | ForEach-Object { Write-Host $_ }
            }
            throw "Command failed with exit code $($process.ExitCode): $FilePath $($startInfo.Arguments)"
        }
        Write-Host ("{0} completed in {1:N1}s." -f $Activity, $elapsed.Elapsed.TotalSeconds)
    } finally {
        if (-not $process.HasExited) { $process.Kill() }
        $process.Dispose()
    }
}

function Wait-RadioifyMsixPackageFile {
    param([Parameter(Mandatory = $true)][string]$Path)

    foreach ($attempt in 1..40) {
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            $item = Get-Item -LiteralPath $Path -ErrorAction Stop
            if ($item.Length -gt 0) {
                return
            }
        }
        Start-Sleep -Milliseconds 250
    }

    throw "MSIX package was not produced: '$Path'"
}

function Assert-RadioifyMsixPackageSigned {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ExpectedThumbprint
    )

    Wait-RadioifyMsixPackageFile -Path $Path
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if (-not $signature.SignerCertificate) {
        throw "MSIX package is not signed: '$Path'"
    }

    if ($signature.SignerCertificate.Thumbprint -ne $ExpectedThumbprint) {
        throw "MSIX package signer thumbprint '$($signature.SignerCertificate.Thumbprint)' does not match expected '$ExpectedThumbprint'."
    }
}

function Ensure-RadioifyMsixDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (Test-Path -LiteralPath $Path) {
        $item = Get-Item -LiteralPath $Path -ErrorAction Stop
        if (-not $item.PSIsContainer) {
            throw "Expected directory path but found a file: '$Path'"
        }
        return
    }

    New-Item -ItemType Directory -Force -Path $Path | Out-Null
}

function New-RadioifyMsixPngAsset {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int]$Size
    )

    Add-Type -AssemblyName System.Drawing
    $bitmap = New-Object System.Drawing.Bitmap $Size, $Size
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit
    $graphics.Clear([System.Drawing.Color]::FromArgb(18, 28, 44))

    $brush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 214, 120))
    $fontSize = [Math]::Max([int]($Size * 0.42), 12)
    $font = New-Object System.Drawing.Font("Segoe UI", $fontSize, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $format = New-Object System.Drawing.StringFormat
    $format.Alignment = [System.Drawing.StringAlignment]::Center
    $format.LineAlignment = [System.Drawing.StringAlignment]::Center
    $rect = New-Object System.Drawing.RectangleF(0, 0, $Size, $Size)
    $graphics.DrawString("R", $font, $brush, $rect, $format)

    $directory = Split-Path -Parent $Path
    if ($directory -and -not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
    }

    $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)

    $format.Dispose()
    $font.Dispose()
    $brush.Dispose()
    $graphics.Dispose()
    $bitmap.Dispose()
}

function Export-RadioifyMsixPngAssetFromIcon {
    param(
        [Parameter(Mandatory = $true)][string]$IconPath,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int]$Size
    )

    Add-Type -AssemblyName System.Drawing

    $directory = Split-Path -Parent $Path
    if ($directory -and -not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
    }

    $icon = $null
    $bitmap = $null
    try {
        $icon = New-Object System.Drawing.Icon($IconPath, $Size, $Size)
        $bitmap = $icon.ToBitmap()
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        if ($bitmap) {
            $bitmap.Dispose()
        }
        if ($icon) {
            $icon.Dispose()
        }
    }
}

function Get-RadioifyMsixPayloadFiles {
    param([Parameter(Mandatory = $true)][string]$IntegrationDir)

    $distRoot = Split-Path -Parent $IntegrationDir
    $files = [System.Collections.Generic.Dictionary[string, System.IO.FileInfo]]::new([StringComparer]::OrdinalIgnoreCase)
    $files.Add("radioify_explorer.dll", (Get-Item -LiteralPath (Join-Path $IntegrationDir "radioify_explorer.dll")))
    $required = @(
        "radioify.exe", "llama-cpp-LICENSE.txt",
        "models\audio_separation\BandIt-v2-MODEL-CARD"
    ) + (Get-RadioifyWindowsMlRuntimeContract).ProductionRuntimeFiles
    foreach ($relative in $required) {
        $files.Add($relative, (Get-Item -LiteralPath (Join-Path $distRoot $relative) -ErrorAction Stop))
    }
    $icon = Get-Item -LiteralPath (Join-Path $distRoot "radioify.ico") -ErrorAction SilentlyContinue
    if ($icon) { $files.Add("radioify.ico", $icon) }

    $bootstrap = Join-Path $distRoot "transcript"
    foreach ($name in Get-RadioifyTranscriptBootstrapFiles) {
        $files.Add("transcript\$name", (Get-Item -LiteralPath (Join-Path $bootstrap $name) -ErrorAction Stop))
    }
    if (-not (Test-Path -LiteralPath (Join-Path $bootstrap "native\msvcp140.dll"))) {
        throw "The speech bootstrap C++ runtime is missing."
    }
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $bootstrap "native") -Filter '*.dll') {
        $files.Add("transcript\native\$($file.Name)", $file)
    }
    return $files
}

function Get-RadioifyMsixInputFingerprint {
    param(
        [Parameter(Mandatory = $true)]$Files,
        [Parameter(Mandatory = $true)]$Digests,
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [Parameter(Mandatory = $true)][string]$Thumbprint,
        [switch]$NoCompression
    )

    # Optional models live outside the package. Hash the actual payload so
    # preserved timestamps cannot hide a changed or damaged file.
    [xml]$manifest = Get-Content -LiteralPath $ManifestPath -Raw
    $manifest.Package.Identity.Version = "0.0.0.0"
    $text = [System.Text.StringBuilder]::new()
    [void]$text.AppendLine($manifest.DocumentElement.OuterXml)
    [void]$text.AppendLine("$Thumbprint|$NoCompression")
    foreach ($scriptName in @("RadioifyWindowsMsixBuild.ps1", "RadioifyWindowsMsixCommon.ps1",
            "RadioifyWindowsRuntimeContract.ps1", "RadioifyWindowsPackageFiles.ps1")) {
        [void]$text.AppendLine((Get-RadioifyFileDigest -Path (Join-Path $PSScriptRoot $scriptName)))
    }
    # Culture-sensitive sorting differs between Windows PowerShell and pwsh.
    # The same payload must have one fingerprint on either host.
    $relativePaths = [string[]]@($Files.Keys)
    [Array]::Sort($relativePaths, [StringComparer]::OrdinalIgnoreCase)
    foreach ($relative in $relativePaths) {
        $file = $Files[$relative]
        [void]$text.AppendLine("$relative|$($file.Length)|$($Digests[$relative])")
    }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text.ToString()))).Replace("-", "")
    } finally { $sha.Dispose() }
}

function Sync-RadioifyMsixPayloadFiles {
    param(
        [Parameter(Mandatory = $true)][string]$LayoutDir,
        [Parameter(Mandatory = $true)]$Files,
        [Parameter(Mandatory = $true)]$Digests
    )

    Ensure-RadioifyMsixDirectory -Path $LayoutDir
    $root = (Get-Item -LiteralPath $LayoutDir -Force).FullName.TrimEnd('\')
    $prefix = $root + '\'
    $existing = @(Get-Item -LiteralPath $root -Force) + @(Get-ChildItem -LiteralPath $root -Recurse -Force)
    # Never follow a link when reconciling this owned staging tree.
    foreach ($item in $existing) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "MSIX staging contains a reparse point: '$($item.FullName)'."
        }
    }
    $copied = 0
    foreach ($relative in $Files.Keys) {
        $destination = [IO.Path]::GetFullPath((Join-Path $root $relative))
        if (-not $destination.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "MSIX payload path escapes the staging directory: '$relative'."
        }
        if (Copy-RadioifyFileIfChanged -Source $Files[$relative].FullName -Destination $destination `
                -SourceDigest $Digests[$relative]) { ++$copied }
    }
    $generated = @("AppxManifest.xml", "Assets\StoreLogo.png", "Assets\SmallLogo.png")
    foreach ($item in $existing) {
        if ($item.PSIsContainer) { continue }
        if (-not $item.FullName.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "MSIX staging file is outside its expected directory: '$($item.FullName)'."
        }
        $relative = $item.FullName.Substring($prefix.Length)
        if (-not $Files.ContainsKey($relative) -and $relative -notin $generated) {
            Remove-Item -LiteralPath $item.FullName -Force
        }
    }
    Write-Host "MSIX staging: $copied files copied, $($Files.Count - $copied) unchanged."
}

function Initialize-RadioifyMsixPackageLayout {
    param(
        [Parameter(Mandatory = $true)][string]$IntegrationDir,
        [string]$PackageVersion,
        [Parameter(Mandatory = $true)]$Files,
        [Parameter(Mandatory = $true)]$Digests
    )

    $layoutDir = Join-Path $IntegrationDir "package-layout"
    $resolvedPackageVersion = Resolve-RadioifyMsixPackageIdentityVersion -Version $PackageVersion
    Sync-RadioifyMsixPayloadFiles -LayoutDir $layoutDir -Files $Files -Digests $Digests

    Copy-Item -LiteralPath (Join-Path $IntegrationDir "Package.appxmanifest") `
        -Destination (Join-Path $layoutDir "AppxManifest.xml") `
        -Force
    Set-RadioifyMsixManifestIdentityVersion `
        -ManifestPath (Join-Path $layoutDir "AppxManifest.xml") `
        -Version $resolvedPackageVersion

    $assetsDir = Join-Path $layoutDir "Assets"
    New-Item -ItemType Directory -Force -Path $assetsDir | Out-Null
    $iconSource = Join-Path $layoutDir "radioify.ico"
    if (Test-Path -LiteralPath $iconSource) {
        Export-RadioifyMsixPngAssetFromIcon `
            -IconPath $iconSource `
            -Path (Join-Path $assetsDir "StoreLogo.png") `
            -Size 150
        Export-RadioifyMsixPngAssetFromIcon `
            -IconPath $iconSource `
            -Path (Join-Path $assetsDir "SmallLogo.png") `
            -Size 44
    } else {
        New-RadioifyMsixPngAsset -Path (Join-Path $assetsDir "StoreLogo.png") -Size 150
        New-RadioifyMsixPngAsset -Path (Join-Path $assetsDir "SmallLogo.png") -Size 44
    }

    return $layoutDir
}

function Get-RadioifyMsixPackageLayoutPath {
    param([Parameter(Mandatory = $true)][string]$IntegrationDir)
    return (Join-Path $IntegrationDir "package-layout")
}

function Ensure-RadioifyMsixSigningCertificate {
    param(
        [Parameter(Mandatory = $true)][string]$Subject,
        [Parameter(Mandatory = $true)][string]$IntegrationDir
    )

    $manifestInfo = Get-RadioifyMsixManifestInfo -IntegrationDir $IntegrationDir
    Clear-RadioifyMsixObsoleteArtifacts `
        -IntegrationDir $IntegrationDir `
        -PackageName $manifestInfo.PackageName
    # Windows PowerShell needs the certificate provider loaded before binding
    # its dynamic -CodeSigningCert parameter in a fresh packaging process.
    Import-Module (Join-Path $PSHOME "Modules\Microsoft.PowerShell.Security\Microsoft.PowerShell.Security.psd1") -ErrorAction Stop
    $cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert -ErrorAction SilentlyContinue |
        Where-Object { $_.Subject -eq $Subject -and $_.HasPrivateKey } |
        Sort-Object NotAfter -Descending |
        Select-Object -First 1

    if (-not $cert) {
        $cert = New-SelfSignedCertificate `
            -Type Custom `
            -Subject $Subject `
            -KeyUsage DigitalSignature `
            -FriendlyName "Radioify Win11 Explorer Development" `
            -CertStoreLocation "Cert:\CurrentUser\My" `
            -KeyAlgorithm RSA `
            -KeyLength 2048 `
            -HashAlgorithm SHA256 `
            -KeyExportPolicy Exportable `
            -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3")
    }

    $certPath = Get-RadioifyMsixCertificatePath `
        -IntegrationDir $IntegrationDir `
        -PackageName $manifestInfo.PackageName
    Export-Certificate -Cert $cert -FilePath $certPath -Force | Out-Null

    return [pscustomobject]@{
        Certificate = $cert
        CertificatePath = $certPath
    }
}

function New-RadioifySignedMsixPackage {
    param(
        [Parameter(Mandatory = $true)][string]$IntegrationDir,
        [string]$PackageVersion,
        [switch]$NoCompression
    )

    $IntegrationDir = (Resolve-Path -LiteralPath $IntegrationDir).Path
    $lock = Enter-RadioifyPackageLock -Directory $IntegrationDir -Name ".msix-build.lock"
    try {
        return Invoke-RadioifySignedMsixPackageBuild -IntegrationDir $IntegrationDir `
            -PackageVersion $PackageVersion -NoCompression:$NoCompression
    } finally { $lock.Dispose() }
}

function Invoke-RadioifySignedMsixPackageBuild {
    param(
        [Parameter(Mandatory = $true)][string]$IntegrationDir,
        [string]$PackageVersion,
        [switch]$NoCompression
    )

    Assert-RadioifyMsixArtifactsExist -IntegrationDir $IntegrationDir
    $manifestInfo = Get-RadioifyMsixManifestInfo -IntegrationDir $IntegrationDir
    Clear-RadioifyMsixObsoleteArtifacts `
        -IntegrationDir $IntegrationDir `
        -PackageName $manifestInfo.PackageName
    $packagePath = Get-RadioifyMsixPackagePath `
        -IntegrationDir $IntegrationDir `
        -PackageName $manifestInfo.PackageName
    $certInfo = Ensure-RadioifyMsixSigningCertificate `
        -Subject $manifestInfo.Publisher `
        -IntegrationDir $IntegrationDir

    Write-Host "Checking MSIX package inputs..."
    $files = Get-RadioifyMsixPayloadFiles -IntegrationDir $IntegrationDir
    $digests = [System.Collections.Generic.Dictionary[string, string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($relative in $files.Keys) {
        $digests.Add($relative, (Get-RadioifyFileDigest -Path $files[$relative].FullName))
    }
    $fingerprint = Get-RadioifyMsixInputFingerprint -Files $files -Digests $digests `
        -ManifestPath $manifestInfo.ManifestPath -Thumbprint $certInfo.Certificate.Thumbprint `
        -NoCompression:$NoCompression
    $statePath = Join-Path $IntegrationDir "package-state.json"
    $state = $null
    try {
        if (Test-Path -LiteralPath $statePath) { $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json }
        if ($state) {
            if ($state.Format -ne 2) { $state = $null }
            else { Resolve-RadioifyMsixPackageIdentityVersion -Version $state.PackageVersion | Out-Null }
        }
        $packageFile = Get-Item -LiteralPath $packagePath -ErrorAction SilentlyContinue
        if ($state -and $state.Format -eq 2 -and $state.InputFingerprint -eq $fingerprint -and
            (-not $PackageVersion -or $state.PackageVersion -eq $PackageVersion) -and
            $packageFile -and $packageFile.Length -eq $state.PackageLength -and
            (Get-RadioifyFileDigest -Path $packagePath) -eq $state.PackageSha256) {
            # State is only saved after successful signing and validation; a
            # changed/missing package, input, version, signer or mode invalidates it.
            Set-RadioifyMsixManifestIdentityVersion -ManifestPath $manifestInfo.ManifestPath -Version $state.PackageVersion
            Write-Host "MSIX unchanged; reusing signed package $($state.PackageVersion)."
            return [pscustomobject]@{
                PackagePath = $packagePath
                CertificatePath = $certInfo.CertificatePath
                LayoutPath = Get-RadioifyMsixPackageLayoutPath -IntegrationDir $IntegrationDir
                ManifestInfo = Get-RadioifyMsixManifestInfo -IntegrationDir $IntegrationDir
                PackageVersion = $state.PackageVersion
                Reused = $true
            }
        }
    } catch {
        # An incomplete/older state file is a cache miss, never a build failure.
        Write-Verbose "MSIX cache record is unusable: $($_.Exception.Message)"
        $state = $null
    }

    $resolvedPackageVersion = Resolve-RadioifyMsixPackageIdentityVersion -Version $PackageVersion
    if (-not $PackageVersion -and $state -and [version]$resolvedPackageVersion -le [version]$state.PackageVersion) {
        $previous = [version]$state.PackageVersion
        if ($previous.Revision -lt 65535) {
            $resolvedPackageVersion = "{0}.{1}.{2}.{3}" -f $previous.Major, $previous.Minor, $previous.Build, ($previous.Revision + 1)
        }
    }
    $makeAppxExe = Resolve-RadioifyWindowsSdkExecutable -ToolName "makeappx.exe"
    $signToolExe = Resolve-RadioifyWindowsSdkExecutable -ToolName "signtool.exe"
    Write-Host "Updating MSIX staging files..."
    $packageLayout = Initialize-RadioifyMsixPackageLayout `
        -IntegrationDir $IntegrationDir -PackageVersion $resolvedPackageVersion -Files $files -Digests $digests
    # Keep the last signed package intact until its replacement has passed validation.
    $pendingPath = "$packagePath.building.msix"
    try {
        $packArguments = @(
            "pack",
            "/d", $packageLayout,
            "/p", $pendingPath,
            "/o"
        )
        if ($NoCompression) { $packArguments += "/nc" }
        $mode = if ($NoCompression) { "without compression" } else { "with compression" }
        Invoke-RadioifyNativeTool -FilePath $makeAppxExe -Arguments $packArguments `
            -Activity "Creating MSIX $mode" -LogPath (Join-Path $IntegrationDir "makeappx.log")
        Wait-RadioifyMsixPackageFile -Path $pendingPath
        Invoke-RadioifyNativeTool -FilePath $signToolExe -Arguments @(
            "sign",
            "/fd", "SHA256",
            "/sha1", $certInfo.Certificate.Thumbprint,
            "/s", "My",
            $pendingPath
        ) -Activity "Signing MSIX" -LogPath (Join-Path $IntegrationDir "signtool.log")
        Write-Host "Verifying signed MSIX..."
        Assert-RadioifyMsixPackageSigned `
            -Path $pendingPath `
            -ExpectedThumbprint $certInfo.Certificate.Thumbprint
        Publish-RadioifyPackageFile -PendingPath $pendingPath -Destination $packagePath
        Set-RadioifyMsixManifestIdentityVersion -ManifestPath $manifestInfo.ManifestPath -Version $resolvedPackageVersion
        $packageFile = Get-Item -LiteralPath $packagePath
        Write-RadioifyPackageState -Path $statePath -State @{
            Format = 2
            InputFingerprint = $fingerprint
            PackageVersion = $resolvedPackageVersion
            PackageLength = $packageFile.Length
            PackageSha256 = Get-RadioifyFileDigest -Path $packagePath
        }
    } finally {
        if ([IO.File]::Exists($pendingPath)) { [IO.File]::Delete($pendingPath) }
    }

    return [pscustomobject]@{
        PackagePath = $packagePath
        CertificatePath = $certInfo.CertificatePath
        LayoutPath = $packageLayout
        ManifestInfo = Get-RadioifyMsixManifestInfo -IntegrationDir $IntegrationDir
        PackageVersion = $resolvedPackageVersion
        Reused = $false
    }
}
