function Assert-ExplicitVcpkgRootValid {
  param([string]$ProvidedRoot)

  if (-not $ProvidedRoot) { return }
  if (-not (Test-Path $ProvidedRoot)) {
    Fail-Build "Explicit -VcpkgRoot path does not exist: $ProvidedRoot"
  }

  $exe = Join-Path $ProvidedRoot "vcpkg.exe"
  if (-not (Test-Path $exe)) {
    Fail-Build "Explicit -VcpkgRoot is missing vcpkg.exe: $ProvidedRoot"
  }
}

function Resolve-VcpkgRoot {
  param([string]$PreferredRoot)

  $vcpkgRoot = $PreferredRoot
  if ($vcpkgRoot -and (Test-Path $vcpkgRoot)) { return $vcpkgRoot }

  $vcpkgRoot = Get-ProcessEnvironmentVariable -Name "VCPKG_ROOT"
  if ($vcpkgRoot -and (Test-Path $vcpkgRoot)) { return $vcpkgRoot }

  $vcpkgCmd = Get-Command vcpkg -ErrorAction SilentlyContinue
  if ($vcpkgCmd) {
    $candidate = Split-Path -Parent $vcpkgCmd.Source
    if ($candidate -and (Test-Path $candidate)) { return $candidate }
  }

  foreach ($candidate in (Get-VisualStudioVcpkgRoots)) {
    if ($candidate -and (Test-Path $candidate)) {
      return $candidate
    }
  }

  return $null
}

function Resolve-VcpkgExe {
  param([string]$VcpkgRoot)

  if ($VcpkgRoot) {
    $exe = Join-Path $VcpkgRoot "vcpkg.exe"
    if (Test-Path $exe) { return $exe }
  }

  $cmd = Get-Command vcpkg -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }

  return $null
}

function Resolve-InstalledRoot {
  param([pscustomobject]$Context)

  $installedDir = Get-ProcessEnvironmentVariable -Name "VCPKG_INSTALLED_DIR"
  if ($installedDir) {
    return $installedDir
  }

  $manifestInstalled = Join-Path $Context.Paths.Root "vcpkg_installed"
  if (Test-Path $manifestInstalled) {
    return $manifestInstalled
  }

  if ($Context.Tools.VcpkgRoot) {
    return (Join-Path $Context.Tools.VcpkgRoot "installed")
  }

  return $null
}

function Assert-VcpkgRootReadableWhenRequired {
  param(
    [string]$VcpkgRoot,
    [string]$VcpkgExe,
    [string]$RepoRoot,
    [bool]$NeedsVcpkg
  )

  if (-not $NeedsVcpkg) { return }
  if ($VcpkgRoot -and (Test-Path $VcpkgRoot) -and $VcpkgExe) { return }

  $visualStudioVcpkgRoots = @(Get-VisualStudioVcpkgRoots)
  $defaultHint = "<Visual Studio>\\VC\\vcpkg"
  if ($visualStudioVcpkgRoots.Count -gt 0) {
    $defaultHint = $visualStudioVcpkgRoots[0]
  }
  $manifestInstalled = Join-Path $RepoRoot "vcpkg_installed"

  Fail-Build @"
vcpkg kon niet automatisch worden gevonden, maar is wel nodig voor deze build.

Waarom dit vaak gebeurt (vooral in WSL2):
- vcpkg.exe staat niet op PATH in deze PowerShell sessie.
- VCPKG_ROOT is niet gezet voor deze sessie/gebruiker.

Zo los je het op:
1) Laat Visual Studio de root dynamisch bepalen:
   `$vswhere = Join-Path `${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
   `$env:VCPKG_ROOT = Join-Path (& `$vswhere -latest -products * -property installationPath) "VC\vcpkg"

2) Eenmalig expliciet meegeven:
   .\build.ps1 -Static -InstallDeps -VcpkgRoot "$defaultHint"

3) Of environment variabele zetten (PowerShell):
   `$env:VCPKG_ROOT="$defaultHint"

4) Persistente user-waarde:
   [Environment]::SetEnvironmentVariable("VCPKG_ROOT", "$defaultHint", "User")

Extra context:
- Script root: $RepoRoot
- Verwachte manifest install map: $manifestInstalled
"@
}
