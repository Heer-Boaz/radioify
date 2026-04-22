Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "RadioifyWindowsShellCommon.ps1")
. (Join-Path $PSScriptRoot "RadioifyWindowsMediaAppRegistration.ps1")

$script:RadioifyWindowsPackageManifestName = "RadioifyPackage.json"
$script:RadioifyWindowsUninstallKeyName = "Radioify"
$script:RadioifyWindowsPublisher = "Radioify"
$script:RadioifyWindowsExplorerIntegrationDirectoryName = "win11-explorer-integration"

function Get-RadioifyWindowsDefaultInstallDirectory {
    $localAppData = [Environment]::GetFolderPath("LocalApplicationData")
    if ([string]::IsNullOrWhiteSpace($localAppData)) {
        throw "Unable to resolve the LocalApplicationData directory."
    }

    return (Join-Path $localAppData "Programs\$script:RadioifyWindowsAppName")
}

function Resolve-RadioifyWindowsPackageRoot {
    param(
        [string]$PackageRoot,
        [string]$ScriptRoot
    )

    $candidate = $PackageRoot
    if (-not $candidate) {
        $candidate = Join-Path $ScriptRoot "..\.."
    }

    $resolved = [System.IO.Path]::GetFullPath($candidate)
    $manifestPath = Join-Path $resolved $script:RadioifyWindowsPackageManifestName
    if (-not (Test-Path -LiteralPath $manifestPath)) {
        throw "Radioify package manifest not found at '$manifestPath'. Extract the packaged bundle first and run the install script from that bundle."
    }

    return $resolved
}

function Read-RadioifyWindowsPackageManifest {
    param([Parameter(Mandatory = $true)][string]$PackageRoot)

    $manifestPath = Join-Path $PackageRoot $script:RadioifyWindowsPackageManifestName
    if (-not (Test-Path -LiteralPath $manifestPath)) {
        throw "Radioify package manifest not found at '$manifestPath'."
    }

    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if (-not $manifest.ExecutableName) {
        throw "Radioify package manifest is missing 'ExecutableName'."
    }

    return $manifest
}

function Get-RadioifyWindowsUninstallRegistryPath {
    return "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Uninstall\$script:RadioifyWindowsUninstallKeyName"
}

function Get-RadioifyWindowsPackageDefinition {
    param(
        [Parameter(Mandatory = $true)][string]$PackageRoot,
        [string]$InstallDir
    )

    $resolvedRoot = Resolve-RadioifyWindowsPackageRoot -PackageRoot $PackageRoot -ScriptRoot $PSScriptRoot
    $manifest = Read-RadioifyWindowsPackageManifest -PackageRoot $resolvedRoot

    if (-not $InstallDir) {
        $InstallDir = Get-RadioifyWindowsDefaultInstallDirectory
    }
    $resolvedInstallDir = [System.IO.Path]::GetFullPath($InstallDir)
    $installedExecutablePath = Join-Path $resolvedInstallDir $manifest.ExecutableName
    $packageExecutablePath = Join-Path $resolvedRoot $manifest.ExecutableName
    if (-not (Test-Path -LiteralPath $packageExecutablePath)) {
        throw "Radioify package is missing '$($manifest.ExecutableName)' at '$packageExecutablePath'."
    }

    return [pscustomobject]@{
        PackageRoot = $resolvedRoot
        Manifest = $manifest
        InstallDir = $resolvedInstallDir
        PackageExecutablePath = $packageExecutablePath
        InstalledExecutablePath = $installedExecutablePath
        InstalledIconPath = Join-Path $resolvedInstallDir "radioify.ico"
        PackageExplorerIntegrationDir = Join-Path $resolvedRoot $script:RadioifyWindowsExplorerIntegrationDirectoryName
        InstalledExplorerIntegrationDir = Join-Path $resolvedInstallDir $script:RadioifyWindowsExplorerIntegrationDirectoryName
        InstalledExplorerInstallScriptPath = Join-Path $resolvedInstallDir "scripts\windows\install_radioify_win11_context_menu.ps1"
        InstalledExplorerUninstallScriptPath = Join-Path $resolvedInstallDir "scripts\windows\uninstall_radioify_win11_context_menu.ps1"
        InstalledUninstallScriptPath = Join-Path $resolvedInstallDir "uninstall_radioify.ps1"
        InstalledUninstallCmdPath = Join-Path $resolvedInstallDir "uninstall_radioify.cmd"
        UninstallRegistryPath = Get-RadioifyWindowsUninstallRegistryPath
    }
}

function Get-RadioifyWindowsInstalledAppDefinition {
    param([string]$InstallDir)

    if (-not $InstallDir) {
        $InstallDir = Get-RadioifyWindowsDefaultInstallDirectory
    }

    $resolvedInstallDir = [System.IO.Path]::GetFullPath($InstallDir)
    $installedExecutablePath = Join-Path $resolvedInstallDir $script:RadioifyWindowsExecutableName

    return [pscustomobject]@{
        InstallDir = $resolvedInstallDir
        InstalledExecutablePath = $installedExecutablePath
        InstalledIconPath = Join-Path $resolvedInstallDir "radioify.ico"
        InstalledExplorerIntegrationDir = Join-Path $resolvedInstallDir $script:RadioifyWindowsExplorerIntegrationDirectoryName
        InstalledExplorerInstallScriptPath = Join-Path $resolvedInstallDir "scripts\windows\install_radioify_win11_context_menu.ps1"
        InstalledExplorerUninstallScriptPath = Join-Path $resolvedInstallDir "scripts\windows\uninstall_radioify_win11_context_menu.ps1"
        InstalledUninstallScriptPath = Join-Path $resolvedInstallDir "uninstall_radioify.ps1"
        InstalledUninstallCmdPath = Join-Path $resolvedInstallDir "uninstall_radioify.cmd"
        UninstallRegistryPath = Get-RadioifyWindowsUninstallRegistryPath
    }
}

function Set-RadioifyWindowsRegistryStringValue {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Value
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }

    New-ItemProperty -LiteralPath $Path -Name $Name -PropertyType String -Value $Value -Force | Out-Null
}

function Set-RadioifyWindowsRegistryDwordValue {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][int]$Value
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }

    New-ItemProperty -LiteralPath $Path -Name $Name -PropertyType DWord -Value $Value -Force | Out-Null
}

function Register-RadioifyWindowsUninstallEntry {
    param([Parameter(Mandatory = $true)]$Definition)

    $manifest = $Definition.Manifest
    $keyPath = $Definition.UninstallRegistryPath
    $installDate = Get-Date -Format "yyyyMMdd"
    $uninstallCommand = ('powershell.exe -NoProfile -ExecutionPolicy Bypass -File "{0}"' -f $Definition.InstalledUninstallScriptPath)

    if (-not (Test-Path -LiteralPath $keyPath)) {
        New-Item -ItemType Directory -Path $keyPath -Force | Out-Null
    }

    Set-RadioifyWindowsRegistryStringValue -Path $keyPath -Name "DisplayName" -Value $manifest.DisplayName
    Set-RadioifyWindowsRegistryStringValue -Path $keyPath -Name "DisplayVersion" -Value $manifest.DisplayVersion
    Set-RadioifyWindowsRegistryStringValue -Path $keyPath -Name "Publisher" -Value $script:RadioifyWindowsPublisher
    Set-RadioifyWindowsRegistryStringValue -Path $keyPath -Name "InstallLocation" -Value $Definition.InstallDir
    Set-RadioifyWindowsRegistryStringValue -Path $keyPath -Name "DisplayIcon" -Value ($Definition.InstalledIconPath + ",0")
    Set-RadioifyWindowsRegistryStringValue -Path $keyPath -Name "UninstallString" -Value $uninstallCommand
    Set-RadioifyWindowsRegistryStringValue -Path $keyPath -Name "QuietUninstallString" -Value $uninstallCommand
    Set-RadioifyWindowsRegistryStringValue -Path $keyPath -Name "InstallDate" -Value $installDate
    Set-RadioifyWindowsRegistryDwordValue -Path $keyPath -Name "NoModify" -Value 1
    Set-RadioifyWindowsRegistryDwordValue -Path $keyPath -Name "NoRepair" -Value 1

    if (Test-Path -LiteralPath $Definition.InstallDir) {
        $sizeBytes = (Get-ChildItem -LiteralPath $Definition.InstallDir -Recurse -File -ErrorAction SilentlyContinue |
            Measure-Object -Property Length -Sum).Sum
        if ($sizeBytes) {
            $estimatedSizeKb = [int][Math]::Ceiling($sizeBytes / 1KB)
            Set-RadioifyWindowsRegistryDwordValue -Path $keyPath -Name "EstimatedSize" -Value $estimatedSizeKb
        }
    }
}

function Unregister-RadioifyWindowsUninstallEntry {
    param([Parameter(Mandatory = $true)]$Definition)

    if (Test-Path -LiteralPath $Definition.UninstallRegistryPath) {
        Remove-Item -LiteralPath $Definition.UninstallRegistryPath -Recurse -Force
    }
}

function Copy-RadioifyWindowsPackageToInstallDirectory {
    param([Parameter(Mandatory = $true)]$Definition)

    if (-not (Test-Path -LiteralPath $Definition.InstallDir)) {
        New-Item -ItemType Directory -Path $Definition.InstallDir -Force | Out-Null
    }

    if ([string]::Equals($Definition.PackageRoot, $Definition.InstallDir, [System.StringComparison]::OrdinalIgnoreCase)) {
        return
    }

    Get-ChildItem -LiteralPath $Definition.PackageRoot -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName `
            -Destination $Definition.InstallDir `
            -Recurse `
            -Force
    }
}

function Invoke-RadioifyWindowsDeferredDirectoryRemoval {
    param([Parameter(Mandatory = $true)][string]$Path)

    $resolvedPath = [System.IO.Path]::GetFullPath($Path)
    $escapedPath = $resolvedPath.Replace("'", "''")
    $tempScriptPath = Join-Path ([System.IO.Path]::GetTempPath()) ("radioify-uninstall-cleanup-{0}.ps1" -f ([guid]::NewGuid().ToString("N")))

    $cleanupScript = @"
Start-Sleep -Milliseconds 700
for (`$i = 0; `$i -lt 40; `$i++) {
    try {
        if (Test-Path -LiteralPath '$escapedPath') {
            Remove-Item -LiteralPath '$escapedPath' -Recurse -Force -ErrorAction Stop
        }
        break
    } catch {
        Start-Sleep -Milliseconds 500
    }
}
Remove-Item -LiteralPath '$($tempScriptPath.Replace("'", "''"))' -Force -ErrorAction SilentlyContinue
"@

    Set-Content -LiteralPath $tempScriptPath -Value $cleanupScript -Encoding UTF8
    Start-Process -FilePath "powershell.exe" `
        -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $tempScriptPath) `
        -WindowStyle Hidden | Out-Null
}
