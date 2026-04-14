Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "RadioifyWindowsShellCommon.ps1")
. (Join-Path $PSScriptRoot "RadioifyWindowsShortcutInterop.ps1")

$script:RadioifyWindowsAppUserModelId = "Radioify.App"
$script:RadioifyWindowsAppName = "Radioify"
$script:RadioifyWindowsAppDescription = "Radioify media player"
$script:RadioifyWindowsExecutableName = "radioify.exe"
$script:RadioifyWindowsShortcutName = "Radioify.lnk"
$script:RadioifyWindowsRegisteredApplicationName = "Radioify"
$script:RadioifyWindowsMediaClientName = "Radioify"

function Resolve-RadioifyWindowsMediaExecutablePath {
    param(
        [string]$ExecutablePath,
        [string]$ScriptRoot,
        [switch]$AllowMissing
    )

    if ($ExecutablePath) {
        $fullPath = [System.IO.Path]::GetFullPath($ExecutablePath)
        if ($AllowMissing -or (Test-Path -LiteralPath $fullPath)) {
            return $fullPath
        }
        throw "Radioify executable not found at '$fullPath'."
    }

    if ($ScriptRoot) {
        $localExecutable = Join-Path $ScriptRoot $script:RadioifyWindowsExecutableName
        if ($AllowMissing -or (Test-Path -LiteralPath $localExecutable)) {
            if (Test-Path -LiteralPath $localExecutable) {
                return (Resolve-Path -LiteralPath $localExecutable).Path
            }
            return [System.IO.Path]::GetFullPath($localExecutable)
        }
    }

    $repoRoot = $null
    if ($ScriptRoot -and (Test-Path -LiteralPath (Join-Path $ScriptRoot "build.ps1"))) {
        $repoRoot = (Resolve-Path -LiteralPath $ScriptRoot).Path
    } else {
        $repoRoot = (Resolve-Path (Join-Path $ScriptRoot "..\..")).Path
    }
    $defaultExecutable = Join-Path $repoRoot "dist\$script:RadioifyWindowsExecutableName"
    if ($AllowMissing -or (Test-Path -LiteralPath $defaultExecutable)) {
        if (Test-Path -LiteralPath $defaultExecutable) {
            return (Resolve-Path -LiteralPath $defaultExecutable).Path
        }
        return [System.IO.Path]::GetFullPath($defaultExecutable)
    }

    throw "Unable to locate '$script:RadioifyWindowsExecutableName'. Build first with .\build.ps1 -Static or pass -ExecutablePath."
}

function Get-RadioifyWindowsMediaAppDefinition {
    param(
        [Parameter(Mandatory = $true)][string]$ExecutablePath,
        [switch]$AllowMissingExecutable
    )

    $resolvedExecutable = [System.IO.Path]::GetFullPath($ExecutablePath)
    if (-not $AllowMissingExecutable -and -not (Test-Path -LiteralPath $resolvedExecutable)) {
        throw "Radioify executable not found at '$resolvedExecutable'."
    }

    $programsDir = [Environment]::GetFolderPath([System.Environment+SpecialFolder]::Programs)
    if ([string]::IsNullOrWhiteSpace($programsDir)) {
        throw "Unable to resolve the per-user Start Menu Programs directory."
    }

    $shortcutPath = Join-Path $programsDir $script:RadioifyWindowsShortcutName
    $executableDirectory = Split-Path -Parent $resolvedExecutable
    $iconLocation = "$resolvedExecutable,0"

    return [pscustomobject]@{
        ExecutablePath = $resolvedExecutable
        ExecutableDirectory = $executableDirectory
        IconLocation = $iconLocation
        ShellOpenCommand = ('"{0}" --shell-open "%1"' -f $resolvedExecutable)
        AppUserModelId = $script:RadioifyWindowsAppUserModelId
        DisplayName = $script:RadioifyWindowsAppName
        Description = $script:RadioifyWindowsAppDescription
        ShortcutPath = $shortcutPath
        AppPathsKey = "Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\App Paths\$script:RadioifyWindowsExecutableName"
        ApplicationsKey = "Registry::HKEY_CURRENT_USER\Software\Classes\Applications\$script:RadioifyWindowsExecutableName"
        DefaultIconKey = "Registry::HKEY_CURRENT_USER\Software\Classes\Applications\$script:RadioifyWindowsExecutableName\DefaultIcon"
        OpenCommandKey = "Registry::HKEY_CURRENT_USER\Software\Classes\Applications\$script:RadioifyWindowsExecutableName\shell\open\command"
        MediaClientKey = "Registry::HKEY_CURRENT_USER\Software\Clients\Media\$script:RadioifyWindowsMediaClientName"
        CapabilitiesKey = "Registry::HKEY_CURRENT_USER\Software\Clients\Media\$script:RadioifyWindowsMediaClientName\Capabilities"
        RegisteredApplicationsKey = "Registry::HKEY_CURRENT_USER\Software\RegisteredApplications"
        RegisteredApplicationsValueName = $script:RadioifyWindowsRegisteredApplicationName
        RegisteredApplicationsValue = "Software\Clients\Media\$script:RadioifyWindowsMediaClientName\Capabilities"
    }
}

function Ensure-RadioifyRegistryKey {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -Path $Path -Force | Out-Null
    }
}

function Set-RadioifyRegistryDefaultValue {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Value
    )

    Ensure-RadioifyRegistryKey -Path $Path
    Set-Item -LiteralPath $Path -Value $Value
}

function Set-RadioifyRegistryStringValue {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Value
    )

    Ensure-RadioifyRegistryKey -Path $Path
    New-ItemProperty -LiteralPath $Path -Name $Name -PropertyType String -Value $Value -Force | Out-Null
}

function Get-RadioifyRegistryDefaultValue {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return $null
    }

    $item = Get-Item -LiteralPath $Path -ErrorAction SilentlyContinue
    if (-not $item) {
        return $null
    }

    return $item.GetValue("", $null)
}

function Get-RadioifyRegistryValue {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return $null
    }

    $item = Get-Item -LiteralPath $Path -ErrorAction SilentlyContinue
    if (-not $item) {
        return $null
    }

    return $item.GetValue($Name, $null)
}

function Remove-RadioifyRegistryTree {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

function Remove-RadioifyRegistryValue {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if (Test-Path -LiteralPath $Path) {
        Remove-ItemProperty -LiteralPath $Path -Name $Name -ErrorAction SilentlyContinue
    }
}

function Get-RadioifyWindowsMediaAppRegistrationState {
    param([Parameter(Mandatory = $true)][string]$ExecutablePath)

    $definition = Get-RadioifyWindowsMediaAppDefinition -ExecutablePath $ExecutablePath
    $shortcutAppUserModelId = $null
    if (Test-Path -LiteralPath $definition.ShortcutPath) {
        $shortcutAppUserModelId = Get-RadioifyWindowsShortcutAppUserModelId -Path $definition.ShortcutPath
    }

    return [pscustomobject]@{
        ExecutablePath = $definition.ExecutablePath
        AppUserModelId = $definition.AppUserModelId
        ShortcutPath = $definition.ShortcutPath
        ShortcutExists = Test-Path -LiteralPath $definition.ShortcutPath
        ShortcutAppUserModelId = $shortcutAppUserModelId
        AppPathsDefault = Get-RadioifyRegistryDefaultValue -Path $definition.AppPathsKey
        FriendlyAppName = Get-RadioifyRegistryValue -Path $definition.ApplicationsKey -Name "FriendlyAppName"
        RegisteredApplicationsValue = Get-RadioifyRegistryValue -Path $definition.RegisteredApplicationsKey -Name $definition.RegisteredApplicationsValueName
    }
}

function Register-RadioifyWindowsMediaApp {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([Parameter(Mandatory = $true)][string]$ExecutablePath)

    Assert-RadioifyWindowsHost
    $definition = Get-RadioifyWindowsMediaAppDefinition -ExecutablePath $ExecutablePath

    if ($PSCmdlet.ShouldProcess($definition.ExecutablePath, "Register Radioify as a Windows media app")) {
        Set-RadioifyRegistryDefaultValue -Path $definition.AppPathsKey -Value $definition.ExecutablePath
        Set-RadioifyRegistryStringValue -Path $definition.AppPathsKey -Name "Path" -Value $definition.ExecutableDirectory

        Set-RadioifyRegistryStringValue -Path $definition.ApplicationsKey -Name "FriendlyAppName" -Value $definition.DisplayName
        Set-RadioifyRegistryDefaultValue -Path $definition.DefaultIconKey -Value $definition.IconLocation
        Set-RadioifyRegistryDefaultValue -Path $definition.OpenCommandKey -Value $definition.ShellOpenCommand

        Set-RadioifyRegistryStringValue -Path $definition.CapabilitiesKey -Name "ApplicationName" -Value $definition.DisplayName
        Set-RadioifyRegistryStringValue -Path $definition.CapabilitiesKey -Name "ApplicationDescription" -Value $definition.Description
        Set-RadioifyRegistryStringValue -Path $definition.RegisteredApplicationsKey -Name $definition.RegisteredApplicationsValueName -Value $definition.RegisteredApplicationsValue

        # A Start Menu shortcut with a matching AppUserModelID lets Windows resolve
        # the unpackaged desktop app to a friendly name in the media flyout.
        New-RadioifyWindowsShortcut `
            -Path $definition.ShortcutPath `
            -TargetPath $definition.ExecutablePath `
            -WorkingDirectory $definition.ExecutableDirectory `
            -Description $definition.Description `
            -IconPath $definition.ExecutablePath `
            -Arguments $null `
            -AppUserModelId $definition.AppUserModelId

        Invoke-RadioifyShellAssociationRefresh
    }

    return (Get-RadioifyWindowsMediaAppRegistrationState -ExecutablePath $definition.ExecutablePath)
}

function Unregister-RadioifyWindowsMediaApp {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([Parameter(Mandatory = $true)][string]$ExecutablePath)

    Assert-RadioifyWindowsHost
    $definition = Get-RadioifyWindowsMediaAppDefinition -ExecutablePath $ExecutablePath -AllowMissingExecutable

    if ($PSCmdlet.ShouldProcess($definition.ExecutablePath, "Unregister Radioify as a Windows media app")) {
        if (Test-Path -LiteralPath $definition.ShortcutPath) {
            Remove-Item -LiteralPath $definition.ShortcutPath -Force
        }

        Remove-RadioifyRegistryValue -Path $definition.RegisteredApplicationsKey -Name $definition.RegisteredApplicationsValueName
        Remove-RadioifyRegistryTree -Path $definition.MediaClientKey
        Remove-RadioifyRegistryTree -Path $definition.ApplicationsKey
        Remove-RadioifyRegistryTree -Path $definition.AppPathsKey

        Invoke-RadioifyShellAssociationRefresh
    }
}
