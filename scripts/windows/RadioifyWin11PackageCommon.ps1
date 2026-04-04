Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Assert-RadioifyWindowsHost {
    $platform = [System.Environment]::OSVersion.Platform
    if ($platform -ne [System.PlatformID]::Win32NT) {
        throw "The Windows 11 Explorer integration scripts must be run on Windows."
    }
}

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
    $manifestText = Get-Content -LiteralPath $manifestPath -Raw
    $surrogateAppId = $null
    $surrogateMatch = [regex]::Match($manifestText, 'SurrogateServer\s+AppId="([^"]+)"')
    if ($surrogateMatch.Success) {
        $surrogateAppId = $surrogateMatch.Groups[1].Value
    }
    return [pscustomobject]@{
        ManifestPath = $manifestPath
        PackageName = [string]$manifestXml.Package.Identity.Name
        Publisher = [string]$manifestXml.Package.Identity.Publisher
        SurrogateAppId = $surrogateAppId
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

function New-RadioifyWin11Directory {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $Path | Out-Null
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

function Initialize-RadioifyWin11DeployRoot {
    param([Parameter(Mandatory = $true)][string]$IntegrationDir)

    $distRoot = Split-Path -Parent $IntegrationDir
    $deployRoot = Join-Path $IntegrationDir "external-location"
    New-RadioifyWin11Directory -Path $deployRoot

    foreach ($item in Get-ChildItem -LiteralPath $distRoot -Force) {
        if ($item.FullName -eq $IntegrationDir) {
            continue
        }

        Copy-Item -LiteralPath $item.FullName `
            -Destination (Join-Path $deployRoot $item.Name) `
            -Recurse -Force
    }

    # The COM SurrogateServer Path="radioify_explorer.dll" is resolved relative
    # to the external location root, so the DLL must live there next to radioify.exe.
    $dllSource = Join-Path $IntegrationDir "radioify_explorer.dll"
    if (Test-Path -LiteralPath $dllSource) {
        Copy-Item -LiteralPath $dllSource `
            -Destination (Join-Path $deployRoot "radioify_explorer.dll") `
            -Force
    }

    return $deployRoot
}

function Initialize-RadioifyWin11PackageLayout {
    param([Parameter(Mandatory = $true)][string]$IntegrationDir)

    $layoutDir = Join-Path $IntegrationDir "package-layout"
    New-RadioifyWin11Directory -Path $layoutDir

    Copy-Item -LiteralPath (Join-Path $IntegrationDir "Package.appxmanifest") `
        -Destination (Join-Path $layoutDir "AppxManifest.xml") `
        -Force
    Copy-Item -LiteralPath (Join-Path $IntegrationDir "radioify_explorer.dll") `
        -Destination (Join-Path $layoutDir "radioify_explorer.dll") `
        -Force

    $assetsDir = Join-Path $layoutDir "Assets"
    New-Item -ItemType Directory -Force -Path $assetsDir | Out-Null
    New-RadioifyWin11PngAsset -Path (Join-Path $assetsDir "StoreLogo.png") -Size 150
    New-RadioifyWin11PngAsset -Path (Join-Path $assetsDir "SmallLogo.png") -Size 44

    return $layoutDir
}

function Get-RadioifyWin11DeployRootPath {
    param([Parameter(Mandatory = $true)][string]$IntegrationDir)
    return (Join-Path $IntegrationDir "external-location")
}

function Get-RadioifyWin11PackageLayoutPath {
    param([Parameter(Mandatory = $true)][string]$IntegrationDir)
    return (Join-Path $IntegrationDir "package-layout")
}

function Get-RadioifyWin11PackagePath {
    param(
        [Parameter(Mandatory = $true)][string]$IntegrationDir,
        [Parameter(Mandatory = $true)][string]$PackageName
    )

    return (Join-Path $IntegrationDir "$PackageName.msix")
}

function Ensure-RadioifyWin11Certificate {
    param(
        [Parameter(Mandatory = $true)][string]$Subject,
        [Parameter(Mandatory = $true)][string]$IntegrationDir
    )

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

    $certPath = Join-Path $IntegrationDir "Radioify.Win11Explorer.cer"
    Export-Certificate -Cert $cert -FilePath $certPath -Force | Out-Null

    $trusted = Get-ChildItem Cert:\LocalMachine\TrustedPeople -ErrorAction SilentlyContinue |
        Where-Object { $_.Thumbprint -eq $cert.Thumbprint } |
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

        Import-Certificate -FilePath $certPath -CertStoreLocation "Cert:\LocalMachine\TrustedPeople" | Out-Null
    }

    return [pscustomobject]@{
        Certificate = $cert
        CertificatePath = $certPath
    }
}

function Invoke-RadioifyShellAssociationRefresh {
    if (-not ("Radioify.ShellNative" -as [type])) {
        Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
namespace Radioify {
    public static class ShellNative {
        [DllImport("shell32.dll")]
        public static extern void SHChangeNotify(uint wEventId, uint uFlags, IntPtr dwItem1, IntPtr dwItem2);
    }
}
"@
    }

    [Radioify.ShellNative]::SHChangeNotify(0x08000000, 0x0000, [IntPtr]::Zero, [IntPtr]::Zero)
}

function Restart-RadioifyExplorerShell {
    $explorerProcesses = Get-Process explorer -ErrorAction SilentlyContinue
    if ($explorerProcesses) {
        $explorerProcesses | Stop-Process -Force
        Wait-RadioifyProcessExit -Name "explorer" | Out-Null
    }
    Start-Process explorer.exe
}

function Start-RadioifyExplorerShell {
    if (-not (Get-Process explorer -ErrorAction SilentlyContinue | Select-Object -First 1)) {
        Start-Process explorer.exe
    }
}

function Stop-RadioifyExplorerShell {
    Get-Process explorer -ErrorAction SilentlyContinue | Stop-Process -Force
}

function Wait-RadioifyProcessExit {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [int]$TimeoutSeconds = 10
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $processes = Get-Process -Name $Name -ErrorAction SilentlyContinue
        if (-not $processes) {
            return $true
        }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $deadline)

    return $false
}

function Stop-RadioifyWin11SurrogateServer {
    param([string]$AppId)

    if ([string]::IsNullOrWhiteSpace($AppId)) {
        return
    }

    $needle = "/Processid:{$AppId}".ToLowerInvariant()
    Get-CimInstance Win32_Process -Filter "Name = 'dllhost.exe'" -ErrorAction SilentlyContinue |
        Where-Object {
            $_.CommandLine -and $_.CommandLine.ToLowerInvariant().Contains($needle)
        } |
        ForEach-Object {
            Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
        }
}

function Stop-RadioifyWin11IntegrationHosts {
    param([string]$SurrogateAppId)

    Stop-RadioifyExplorerShell
    Stop-RadioifyWin11SurrogateServer -AppId $SurrogateAppId
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
        [int]$TimeoutSeconds = 30,
        [switch]$KeepShellStopped,
        [string]$SurrogateAppId
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        if ($KeepShellStopped) {
            Stop-RadioifyWin11IntegrationHosts -SurrogateAppId $SurrogateAppId
        }

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

function Wait-RadioifyWin11PackageSettled {
    param(
        [Parameter(Mandatory = $true)][string]$PackageName,
        [int]$TimeoutSeconds = 30,
        [switch]$KeepShellStopped,
        [string]$SurrogateAppId
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        if ($KeepShellStopped) {
            Stop-RadioifyWin11IntegrationHosts -SurrogateAppId $SurrogateAppId
        }

        $package = Get-InstalledRadioifyWin11Package -PackageName $PackageName
        if (-not $package) {
            return $null
        }

        if (-not (Test-RadioifyWin11PackageBusy -Package $package)) {
            return $package
        }

        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $deadline)

    $package = Get-InstalledRadioifyWin11Package -PackageName $PackageName
    $statusText = if ($package) { [string]$package.Status } else { "not installed" }
    throw "Timed out waiting for package '$PackageName' to settle. Current status: $statusText"
}

function Install-RadioifyWin11Package {
    param(
        [Parameter(Mandatory = $true)][string]$PackagePath,
        [Parameter(Mandatory = $true)][string]$ExternalLocation
    )

    $cmdlet = Resolve-RadioifyWin11AppxCmdlet -Name "Add-AppxPackage"
    & $cmdlet -ForceApplicationShutdown -ForceUpdateFromAnyVersion -Path $PackagePath -ExternalLocation $ExternalLocation
}

function Remove-RadioifyWin11Package {
    param([Parameter(Mandatory = $true)][string]$PackageFullName)

    $cmdlet = Resolve-RadioifyWin11AppxCmdlet -Name "Remove-AppxPackage"
    & $cmdlet -Package $PackageFullName
}
