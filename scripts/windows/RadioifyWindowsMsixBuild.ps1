Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "RadioifyWindowsMsixCommon.ps1")

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

function Invoke-RadioifyNativeTool {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    $global:LASTEXITCODE = 0
    & $FilePath @Arguments
    $exitCode = $global:LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "Command failed: $FilePath $($Arguments -join ' ')"
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

function Reset-RadioifyMsixStagingDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)

    Ensure-RadioifyMsixDirectory -Path $Path

    Get-ChildItem -LiteralPath $Path -Force -ErrorAction SilentlyContinue |
        Remove-Item -Recurse -Force
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

function Initialize-RadioifyMsixPackageLayout {
    param(
        [Parameter(Mandatory = $true)][string]$IntegrationDir,
        [string]$PackageVersion
    )

    $layoutDir = Join-Path $IntegrationDir "package-layout"
    $distRoot = Split-Path -Parent $IntegrationDir
    $resolvedPackageVersion = Resolve-RadioifyMsixPackageIdentityVersion -Version $PackageVersion
    Set-RadioifyMsixManifestIdentityVersion `
        -ManifestPath (Join-Path $IntegrationDir "Package.appxmanifest") `
        -Version $resolvedPackageVersion
    Reset-RadioifyMsixStagingDirectory -Path $layoutDir

    Copy-Item -LiteralPath (Join-Path $IntegrationDir "Package.appxmanifest") `
        -Destination (Join-Path $layoutDir "AppxManifest.xml") `
        -Force
    Copy-Item -LiteralPath (Join-Path $IntegrationDir "radioify_explorer.dll") `
        -Destination (Join-Path $layoutDir "radioify_explorer.dll") `
        -Force
    Copy-Item -LiteralPath (Join-Path $distRoot "radioify.exe") `
        -Destination (Join-Path $layoutDir "radioify.exe") `
        -Force

    $icoSource = Join-Path $distRoot "radioify.ico"
    if (Test-Path -LiteralPath $icoSource) {
        Copy-Item -LiteralPath $icoSource `
            -Destination (Join-Path $layoutDir "radioify.ico") `
            -Force
    }

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
        [string]$PackageVersion
    )

    Assert-RadioifyMsixArtifactsExist -IntegrationDir $IntegrationDir

    $resolvedPackageVersion = Resolve-RadioifyMsixPackageIdentityVersion -Version $PackageVersion
    Set-RadioifyMsixManifestIdentityVersion `
        -ManifestPath (Join-Path $IntegrationDir "Package.appxmanifest") `
        -Version $resolvedPackageVersion

    $manifestInfo = Get-RadioifyMsixManifestInfo -IntegrationDir $IntegrationDir
    Clear-RadioifyMsixObsoleteArtifacts `
        -IntegrationDir $IntegrationDir `
        -PackageName $manifestInfo.PackageName
    $makeAppxExe = Resolve-RadioifyWindowsSdkExecutable -ToolName "makeappx.exe"
    $signToolExe = Resolve-RadioifyWindowsSdkExecutable -ToolName "signtool.exe"
    $packageLayout = Initialize-RadioifyMsixPackageLayout `
        -IntegrationDir $IntegrationDir `
        -PackageVersion $resolvedPackageVersion
    $packagePath = Get-RadioifyMsixPackagePath `
        -IntegrationDir $IntegrationDir `
        -PackageName $manifestInfo.PackageName
    $certInfo = Ensure-RadioifyMsixSigningCertificate `
        -Subject $manifestInfo.Publisher `
        -IntegrationDir $IntegrationDir

    if (Test-Path -LiteralPath $packagePath) {
        Remove-Item -LiteralPath $packagePath -Force
    }
    Invoke-RadioifyNativeTool -FilePath $makeAppxExe -Arguments @(
        "pack",
        "/d", $packageLayout,
        "/p", $packagePath,
        "/o"
    ) | Out-Host
    Invoke-RadioifyNativeTool -FilePath $signToolExe -Arguments @(
        "sign",
        "/fd", "SHA256",
        "/sha1", $certInfo.Certificate.Thumbprint,
        "/s", "My",
        $packagePath
    ) | Out-Host

    return [pscustomobject]@{
        PackagePath = $packagePath
        CertificatePath = $certInfo.CertificatePath
        LayoutPath = $packageLayout
        ManifestInfo = $manifestInfo
        PackageVersion = $resolvedPackageVersion
    }
}
