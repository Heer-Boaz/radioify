function Resolve-StaticTriplet {
  param([string]$Arch)
  if (-not $Arch) { $Arch = Resolve-TargetArch }
  return "$Arch-windows-static"
}

function Is-StaticTriplet {
  param([string]$Triplet)
  if (-not $Triplet) { return $false }
  return $Triplet -match "-static"
}

function Convert-ToStaticTriplet {
  param([string]$Triplet)
  if (-not $Triplet) { return Resolve-StaticTriplet }
  if (Is-StaticTriplet $Triplet) { return $Triplet }
  if ($Triplet -match "^(.*)-windows-md$") {
    return "$($matches[1])-windows-static-md"
  }
  if ($Triplet -match "^(.*)-windows$") {
    return "$($matches[1])-windows-static"
  }
  return Resolve-StaticTriplet
}

function Resolve-BuildTriplet {
  param([bool]$PreferStatic = $false)

  $targetTriplet = Get-ProcessEnvironmentVariable -Name "VCPKG_TARGET_TRIPLET"
  if ($targetTriplet) {
    return $targetTriplet
  }
  $defaultTriplet = Get-ProcessEnvironmentVariable -Name "VCPKG_DEFAULT_TRIPLET"
  if ($defaultTriplet) {
    return $defaultTriplet
  }
  if ($PreferStatic) {
    return Resolve-StaticTriplet
  }
  return "$(Resolve-TargetArch)-windows"
}

function Set-BuildTripletEnvironment {
  param([pscustomobject]$Options)

  $tripletStaticRequested = $false
  if ($Options.Static) {
    $requestedTriplet = Get-ProcessEnvironmentVariable -Name "VCPKG_TARGET_TRIPLET"
    $staticTriplet = Convert-ToStaticTriplet $requestedTriplet
    if ($requestedTriplet -and ($staticTriplet -ne $requestedTriplet)) {
      Write-Host "Static build: overriding VCPKG_TARGET_TRIPLET '$requestedTriplet' -> '$staticTriplet'"
    }
    Set-ProcessEnvironmentVariable -Name "VCPKG_TARGET_TRIPLET" -Value $staticTriplet

    $requestedDefaultTriplet = Get-ProcessEnvironmentVariable -Name "VCPKG_DEFAULT_TRIPLET"
    if (-not $requestedDefaultTriplet -or -not (Is-StaticTriplet $requestedDefaultTriplet)) {
      if ($requestedDefaultTriplet -and ($requestedDefaultTriplet -ne $staticTriplet)) {
        Write-Host "Static build: overriding VCPKG_DEFAULT_TRIPLET '$requestedDefaultTriplet' -> '$staticTriplet'"
      }
      Set-ProcessEnvironmentVariable -Name "VCPKG_DEFAULT_TRIPLET" -Value $staticTriplet
    }

    $tripletStaticRequested = $true
  }

  $effectiveTargetTriplet = Resolve-BuildTriplet -PreferStatic:$Options.Static
  $effectiveDefaultTriplet = Get-ProcessEnvironmentVariable -Name "VCPKG_DEFAULT_TRIPLET"
  if (-not $effectiveDefaultTriplet) {
    $effectiveDefaultTriplet = $effectiveTargetTriplet
  }

  return [pscustomobject]@{
    TripletStaticRequested = $tripletStaticRequested
    EffectiveTargetTriplet = $effectiveTargetTriplet
    EffectiveDefaultTriplet = $effectiveDefaultTriplet
  }
}
