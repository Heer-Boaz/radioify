Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "RadioifyWindowsShellCommon.ps1")

function Test-RadioifyAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Resolve-RadioifyWin11AppxCmdlet {
    param([Parameter(Mandatory = $true)][string]$Name)

    $command = Get-Command -Name $Name -CommandType Cmdlet -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) {
        return $command
    }

    Import-Module Appx -DisableNameChecking -ErrorAction Stop
    $command = Get-Command -Name $Name -CommandType Cmdlet -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) {
        return $command
    }

    throw "Required Appx cmdlet '$Name' was not found."
}

function Resolve-RadioifyWindowsPowerShellExecutable {
    $systemRoot = [Environment]::GetEnvironmentVariable("SystemRoot")
    if (-not [string]::IsNullOrWhiteSpace($systemRoot)) {
        $candidate = Join-Path $systemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    $command = Get-Command powershell.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command -and $command.Source) {
        return $command.Source
    }

    throw "Windows PowerShell was not found. MSIX deployment requires the Windows Appx PowerShell module."
}

function Invoke-RadioifyWin11DeploymentPowerShell {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$ScriptBlock,
        [hashtable]$Parameters,
        [Parameter(Mandatory = $true)][string]$OperationName,
        [int]$TimeoutSeconds = 180
    )

    $tempRoot = [System.IO.Path]::GetTempPath()
    $operationId = [guid]::NewGuid().ToString("N")
    $payloadPath = Join-Path $tempRoot "radioify-appx-$operationId.json"
    $stdoutPath = Join-Path $tempRoot "radioify-appx-$operationId.out"
    $stderrPath = Join-Path $tempRoot "radioify-appx-$operationId.err"
    $payloadLiteral = $payloadPath.Replace("'", "''")

    $payload = [ordered]@{
        Script = $ScriptBlock.ToString()
        Parameters = if ($Parameters) { $Parameters } else { @{} }
    }
    $payload | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $payloadPath -Encoding UTF8

    $bootstrap = @"
`$ErrorActionPreference = "Stop"
`$ProgressPreference = "SilentlyContinue"
`$payload = Get-Content -LiteralPath '$payloadLiteral' -Raw | ConvertFrom-Json
`$params = @{}
if (`$payload.Parameters) {
    foreach (`$entry in `$payload.Parameters.PSObject.Properties) {
        `$params[`$entry.Name] = `$entry.Value
    }
}
`$scriptBlock = [ScriptBlock]::Create([string]`$payload.Script)
& `$scriptBlock @params
"@
    $encodedCommand = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($bootstrap))
    $powershellExe = Resolve-RadioifyWindowsPowerShellExecutable
    $argumentList = @(
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-EncodedCommand",
        $encodedCommand
    )

    $process = $null
    try {
        $process = Start-Process `
            -FilePath $powershellExe `
            -ArgumentList $argumentList `
            -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath `
            -WindowStyle Hidden `
            -PassThru
        $completed = $process.WaitForExit($TimeoutSeconds * 1000)
        if (-not $completed) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            throw "$OperationName did not finish within $TimeoutSeconds seconds. The Windows AppX deployment service is still busy; the installer stopped waiting instead of deadlocking."
        }

        $process.Refresh()
        $stdout = if (Test-Path -LiteralPath $stdoutPath) {
            Get-Content -LiteralPath $stdoutPath -Raw
        } else {
            ""
        }
        $stderr = if (Test-Path -LiteralPath $stderrPath) {
            Get-Content -LiteralPath $stderrPath -Raw
        } else {
            ""
        }

        if ($process.ExitCode -ne 0) {
            $details = (($stderr, $stdout) |
                Where-Object { -not [string]::IsNullOrWhiteSpace($_) }) -join [Environment]::NewLine
            if ($details) {
                throw "$OperationName failed with exit code $($process.ExitCode).`n$details"
            }
            throw "$OperationName failed with exit code $($process.ExitCode)."
        }

        if (-not [string]::IsNullOrWhiteSpace($stdout)) {
            Write-Host ($stdout.Trim())
        }
    } finally {
        if ($process) {
            $process.Dispose()
        }
        foreach ($path in @($payloadPath, $stdoutPath, $stderrPath)) {
            Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
        }
    }
}

function Resolve-RadioifyWin11IntegrationDirectory {
    param(
        [string]$IntegrationDir,
        [string]$ScriptRoot
    )

    if ($IntegrationDir) {
        return (Resolve-Path -LiteralPath $IntegrationDir).Path
    }

    $localManifest = Join-Path $ScriptRoot "Package.appxmanifest"
    if (Test-Path -LiteralPath $localManifest) {
        return (Resolve-Path -LiteralPath $ScriptRoot).Path
    }

    $repoRoot = (Resolve-Path (Join-Path $ScriptRoot "..\\..")).Path
    $candidate = Join-Path $repoRoot "dist\\win11-explorer-integration"
    if (Test-Path -LiteralPath $candidate) {
        return (Resolve-Path -LiteralPath $candidate).Path
    }

    throw "Unable to locate the Win11 Explorer integration directory. Build first with .\\build.ps1 -Static -Win11ExplorerIntegration or pass -IntegrationDir."
}

function Get-RadioifyWin11ManifestInfo {
    param([Parameter(Mandatory = $true)][string]$IntegrationDir)

    $manifestPath = Join-Path $IntegrationDir "Package.appxmanifest"
    if (-not (Test-Path -LiteralPath $manifestPath)) {
        throw "Package manifest not found at '$manifestPath'. Build first with .\\build.ps1 -Static -Win11ExplorerIntegration."
    }

    [xml]$manifestXml = Get-Content -LiteralPath $manifestPath
    return [pscustomobject]@{
        ManifestPath = $manifestPath
        PackageName = [string]$manifestXml.Package.Identity.Name
        Publisher = [string]$manifestXml.Package.Identity.Publisher
        Version = [string]$manifestXml.Package.Identity.Version
    }
}

function New-RadioifyWin11PackageIdentityVersion {
    $now = Get-Date
    return "{0}.{1}.{2}.{3}" -f `
        $now.Year,
        (($now.Month * 100) + $now.Day),
        (($now.Hour * 100) + $now.Minute),
        $now.Second
}

function Resolve-RadioifyWin11PackageIdentityVersion {
    param([string]$Version)

    if ([string]::IsNullOrWhiteSpace($Version)) {
        $Version = New-RadioifyWin11PackageIdentityVersion
    }

    $parts = $Version -split '\.'
    if ($parts.Count -ne 4) {
        throw "MSIX package identity version must have four numeric parts: '$Version'."
    }

    foreach ($part in $parts) {
        $value = 0
        if (-not [int]::TryParse($part, [ref]$value) -or $value -lt 0 -or $value -gt 65535) {
            throw "MSIX package identity version part '$part' is invalid in '$Version'. Expected 0..65535."
        }
    }

    return ($parts -join ".")
}

function Set-RadioifyWin11ManifestIdentityVersion {
    param(
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [Parameter(Mandatory = $true)][string]$Version
    )

    $resolvedVersion = Resolve-RadioifyWin11PackageIdentityVersion -Version $Version
    [xml]$manifestXml = Get-Content -LiteralPath $ManifestPath
    $manifestXml.Package.Identity.Version = $resolvedVersion

    $settings = New-Object System.Xml.XmlWriterSettings
    $settings.Indent = $true
    $settings.Encoding = New-Object System.Text.UTF8Encoding($false)

    $writer = [System.Xml.XmlWriter]::Create($ManifestPath, $settings)
    try {
        $manifestXml.Save($writer)
    } finally {
        $writer.Close()
    }
}

function Assert-RadioifyWin11ArtifactsExist {
    param([Parameter(Mandatory = $true)][string]$IntegrationDir)

    $requiredPaths = @(
        (Join-Path $IntegrationDir "Package.appxmanifest"),
        (Join-Path $IntegrationDir "radioify_explorer.dll"),
        (Join-Path (Split-Path -Parent $IntegrationDir) "radioify.exe")
    )

    foreach ($path in $requiredPaths) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Required Win11 Explorer integration artifact is missing: '$path'. Build first with .\\build.ps1 -Static -Win11ExplorerIntegration."
        }
    }
}

function Resolve-WindowsSdkExecutable {
    param([Parameter(Mandatory = $true)][string]$ToolName)

    $command = Get-Command $ToolName -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command -and $command.Source) {
        return $command.Source
    }

    $searchRoots = @()
    if ($env:ProgramFiles) {
        $searchRoots += (Join-Path $env:ProgramFiles "Windows Kits\\10\\bin")
    }
    $programFilesX86 = [Environment]::GetEnvironmentVariable("ProgramFiles(x86)")
    if ($programFilesX86) {
        $searchRoots += (Join-Path $programFilesX86 "Windows Kits\\10\\bin")
    }

    foreach ($root in ($searchRoots | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $versionDirs = Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending
        foreach ($versionDir in $versionDirs) {
            foreach ($arch in @("x64", "x86")) {
                $candidate = Join-Path $versionDir.FullName "$arch\\$ToolName"
                if (Test-Path -LiteralPath $candidate) {
                    return $candidate
                }
            }
        }
    }

    throw "Windows SDK tool '$ToolName' was not found."
}

function Invoke-NativeTool {
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

function Ensure-RadioifyWin11Directory {
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

function Reset-RadioifyWin11StagingDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)

    Ensure-RadioifyWin11Directory -Path $Path

    Get-ChildItem -LiteralPath $Path -Force -ErrorAction SilentlyContinue |
        Remove-Item -Recurse -Force
}

function New-RadioifyWin11PngAsset {
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

function Export-RadioifyWin11PngAssetFromIcon {
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

function Initialize-RadioifyWin11PackageLayout {
    param(
        [Parameter(Mandatory = $true)][string]$IntegrationDir,
        [string]$PackageVersion
    )

    $layoutDir = Join-Path $IntegrationDir "package-layout"
    $distRoot = Split-Path -Parent $IntegrationDir
    $resolvedPackageVersion = Resolve-RadioifyWin11PackageIdentityVersion -Version $PackageVersion
    Set-RadioifyWin11ManifestIdentityVersion `
        -ManifestPath (Join-Path $IntegrationDir "Package.appxmanifest") `
        -Version $resolvedPackageVersion
    Reset-RadioifyWin11StagingDirectory -Path $layoutDir

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
        Export-RadioifyWin11PngAssetFromIcon `
            -IconPath $iconSource `
            -Path (Join-Path $assetsDir "StoreLogo.png") `
            -Size 150
        Export-RadioifyWin11PngAssetFromIcon `
            -IconPath $iconSource `
            -Path (Join-Path $assetsDir "SmallLogo.png") `
            -Size 44
    } else {
        New-RadioifyWin11PngAsset -Path (Join-Path $assetsDir "StoreLogo.png") -Size 150
        New-RadioifyWin11PngAsset -Path (Join-Path $assetsDir "SmallLogo.png") -Size 44
    }

    return $layoutDir
}

function Get-RadioifyWin11PackageLayoutPath {
    param([Parameter(Mandatory = $true)][string]$IntegrationDir)
    return (Join-Path $IntegrationDir "package-layout")
}

function Get-RadioifyWin11PackageArtifactDirectory {
    param([Parameter(Mandatory = $true)][string]$IntegrationDir)
    return (Split-Path -Parent $IntegrationDir)
}

function Get-RadioifyWin11PackagePath {
    param(
        [Parameter(Mandatory = $true)][string]$IntegrationDir,
        [Parameter(Mandatory = $true)][string]$PackageName
    )

    return (Join-Path (Get-RadioifyWin11PackageArtifactDirectory -IntegrationDir $IntegrationDir) "$PackageName.msix")
}

function Get-RadioifyWin11CertificatePath {
    param(
        [Parameter(Mandatory = $true)][string]$IntegrationDir,
        [Parameter(Mandatory = $true)][string]$PackageName
    )

    return (Join-Path (Get-RadioifyWin11PackageArtifactDirectory -IntegrationDir $IntegrationDir) "$PackageName.cer")
}

function Clear-RadioifyWin11LegacyPackageArtifacts {
    param(
        [Parameter(Mandatory = $true)][string]$IntegrationDir,
        [Parameter(Mandatory = $true)][string]$PackageName
    )

    foreach ($legacyPath in @(
            (Join-Path $IntegrationDir "$PackageName.msix"),
            (Join-Path $IntegrationDir "$PackageName.cer"),
            (Join-Path $IntegrationDir "external-location")
        )) {
        if (Test-Path -LiteralPath $legacyPath) {
            Remove-Item -LiteralPath $legacyPath -Recurse -Force
        }
    }
}

function Ensure-RadioifyWin11SigningCertificate {
    param(
        [Parameter(Mandatory = $true)][string]$Subject,
        [Parameter(Mandatory = $true)][string]$IntegrationDir
    )

    $manifestInfo = Get-RadioifyWin11ManifestInfo -IntegrationDir $IntegrationDir
    Clear-RadioifyWin11LegacyPackageArtifacts `
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

    $certPath = Get-RadioifyWin11CertificatePath `
        -IntegrationDir $IntegrationDir `
        -PackageName $manifestInfo.PackageName
    Export-Certificate -Cert $cert -FilePath $certPath -Force | Out-Null

    return [pscustomobject]@{
        Certificate = $cert
        CertificatePath = $certPath
    }
}

function Ensure-RadioifyWin11CertificateTrusted {
    param(
        [Parameter(Mandatory = $true)][string]$CertificatePath
    )

    if (-not (Test-Path -LiteralPath $CertificatePath)) {
        throw "MSIX certificate not found at '$CertificatePath'."
    }
    $certificate = Get-PfxCertificate -FilePath $CertificatePath
    $trusted = Get-ChildItem Cert:\LocalMachine\TrustedPeople -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Thumbprint -eq $certificate.Thumbprint
        } |
        Select-Object -First 1
    if (-not $trusted) {
        if (-not (Test-RadioifyAdministrator)) {
            throw @"
Installing the Radioify Windows 11 Explorer integration requires an elevated PowerShell session.

The self-signed MSIX package certificate must be trusted in:
  Cert:\LocalMachine\TrustedPeople

Re-run:
  powershell -ExecutionPolicy Bypass -File .\install_win11_explorer_integration.ps1

from an elevated PowerShell window.
"@
        }

        Import-Certificate -FilePath $CertificatePath -CertStoreLocation "Cert:\LocalMachine\TrustedPeople" | Out-Null
    }
}

function Ensure-RadioifyWin11Certificate {
    param(
        [Parameter(Mandatory = $true)][string]$Subject,
        [Parameter(Mandatory = $true)][string]$IntegrationDir
    )

    $certInfo = Ensure-RadioifyWin11SigningCertificate `
        -Subject $Subject `
        -IntegrationDir $IntegrationDir
    Ensure-RadioifyWin11CertificateTrusted -CertificatePath $certInfo.CertificatePath

    return [pscustomobject]@{
        Certificate = $certInfo.Certificate
        CertificatePath = $certInfo.CertificatePath
    }
}

function New-RadioifyWin11SignedPackage {
    param(
        [Parameter(Mandatory = $true)][string]$IntegrationDir,
        [string]$PackageVersion,
        [switch]$TrustCertificate
    )

    Assert-RadioifyWin11ArtifactsExist -IntegrationDir $IntegrationDir

    $resolvedPackageVersion = Resolve-RadioifyWin11PackageIdentityVersion -Version $PackageVersion
    Set-RadioifyWin11ManifestIdentityVersion `
        -ManifestPath (Join-Path $IntegrationDir "Package.appxmanifest") `
        -Version $resolvedPackageVersion

    $manifestInfo = Get-RadioifyWin11ManifestInfo -IntegrationDir $IntegrationDir
    Clear-RadioifyWin11LegacyPackageArtifacts `
        -IntegrationDir $IntegrationDir `
        -PackageName $manifestInfo.PackageName
    $makeAppxExe = Resolve-WindowsSdkExecutable -ToolName "makeappx.exe"
    $signToolExe = Resolve-WindowsSdkExecutable -ToolName "signtool.exe"
    $packageLayout = Initialize-RadioifyWin11PackageLayout `
        -IntegrationDir $IntegrationDir `
        -PackageVersion $resolvedPackageVersion
    $packagePath = Get-RadioifyWin11PackagePath `
        -IntegrationDir $IntegrationDir `
        -PackageName $manifestInfo.PackageName
    $certInfo = Ensure-RadioifyWin11SigningCertificate `
        -Subject $manifestInfo.Publisher `
        -IntegrationDir $IntegrationDir

    if (Test-Path -LiteralPath $packagePath) {
        Remove-Item -LiteralPath $packagePath -Force
    }
    Invoke-NativeTool -FilePath $makeAppxExe -Arguments @(
        "pack",
        "/d", $packageLayout,
        "/p", $packagePath,
        "/o"
    ) | Out-Host
    Invoke-NativeTool -FilePath $signToolExe -Arguments @(
        "sign",
        "/fd", "SHA256",
        "/sha1", $certInfo.Certificate.Thumbprint,
        "/s", "My",
        $packagePath
    ) | Out-Host

    if ($TrustCertificate) {
        Ensure-RadioifyWin11CertificateTrusted -CertificatePath $certInfo.CertificatePath
    }

    return [pscustomobject]@{
        PackagePath = $packagePath
        CertificatePath = $certInfo.CertificatePath
        LayoutPath = $packageLayout
        ManifestInfo = $manifestInfo
        PackageVersion = $resolvedPackageVersion
    }
}

function Ensure-RadioifyWin11PackagedMsixTrusted {
    param([Parameter(Mandatory = $true)][string]$IntegrationDir)

    $manifestInfo = Get-RadioifyWin11ManifestInfo -IntegrationDir $IntegrationDir
    $certificatePath = Get-RadioifyWin11CertificatePath `
        -IntegrationDir $IntegrationDir `
        -PackageName $manifestInfo.PackageName
    if (-not (Test-Path -LiteralPath $certificatePath)) {
        throw "MSIX certificate not found at '$certificatePath'. Repackage Radioify so the signed .msix and .cer are bundled together."
    }

    Ensure-RadioifyWin11CertificateTrusted -CertificatePath $certificatePath
    return $certificatePath
}

function Get-InstalledRadioifyWin11Package {
    param([Parameter(Mandatory = $true)][string]$PackageName)

    $cmdlet = Resolve-RadioifyWin11AppxCmdlet -Name "Get-AppxPackage"
    return (& $cmdlet -Name $PackageName -ErrorAction SilentlyContinue | Select-Object -First 1)
}

function Test-RadioifyWin11PackageBusy {
    param([Parameter(Mandatory = $true)]$Package)

    if (-not $Package) {
        return $false
    }

    $statusText = [string]$Package.Status
    return ($statusText -match 'DeploymentInProgress|Servicing')
}

function Wait-RadioifyWin11PackageState {
    param(
        [Parameter(Mandatory = $true)][string]$PackageName,
        [Parameter(Mandatory = $true)][ValidateSet("Absent", "Ready")][string]$DesiredState,
        [int]$TimeoutSeconds = 30
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $package = Get-InstalledRadioifyWin11Package -PackageName $PackageName
        if ($DesiredState -eq "Absent") {
            if (-not $package) {
                return
            }
        } else {
            if ($package -and -not (Test-RadioifyWin11PackageBusy -Package $package)) {
                return
            }
        }

        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $deadline)

    $package = Get-InstalledRadioifyWin11Package -PackageName $PackageName
    if ($DesiredState -eq "Absent") {
        $statusText = if ($package) { [string]$package.Status } else { "present" }
        throw "Timed out waiting for package '$PackageName' to be removed. Current status: $statusText"
    }

    $statusText = if ($package) { [string]$package.Status } else { "not installed" }
    throw "Timed out waiting for package '$PackageName' to become ready. Current status: $statusText"
}

function Install-RadioifyWin11Package {
    param(
        [Parameter(Mandatory = $true)][string]$PackagePath
    )

    Invoke-RadioifyWin11DeploymentPowerShell `
        -OperationName "Install Radioify MSIX package" `
        -TimeoutSeconds 180 `
        -ScriptBlock {
            param([Parameter(Mandatory = $true)][string]$PackagePath)

            Add-AppxPackage `
                -Path $PackagePath `
                -ForceTargetApplicationShutdown `
                -ForceUpdateFromAnyVersion `
                -DeferRegistrationWhenPackagesAreInUse `
                -ErrorAction Stop
        } `
        -Parameters @{
            PackagePath = $PackagePath
        }
}

function Remove-RadioifyWin11Package {
    param(
        [Parameter(Mandatory = $true)][string]$PackageFullName,
        [string]$PackageName
    )

    try {
        Invoke-RadioifyWin11DeploymentPowerShell `
            -OperationName "Remove Radioify MSIX package" `
            -TimeoutSeconds 180 `
            -ScriptBlock {
                param([Parameter(Mandatory = $true)][string]$PackageFullName)

                Remove-AppxPackage `
                    -Package $PackageFullName `
                    -ErrorAction Stop
            } `
            -Parameters @{
                PackageFullName = $PackageFullName
            }
    } catch {
        if (-not [string]::IsNullOrWhiteSpace($PackageName)) {
            $remainingPackage = Get-InstalledRadioifyWin11Package -PackageName $PackageName
            if (-not $remainingPackage) {
                Write-Warning "Remove-AppxPackage reported an error after '$PackageFullName' was already removed. Continuing."
                return
            }
        }

        throw
    }
}
