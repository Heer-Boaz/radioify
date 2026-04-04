function Invoke-RadioifyBuild {
  param([pscustomobject]$Context)

  Initialize-BuildTools -Context $Context
  if (Invoke-CleanStep -Context $Context) {
    return 0
  }

  Initialize-BuildDependencies -Context $Context

  $configureExit = Invoke-ConfigureStep -Context $Context
  if ($configureExit -ne 0) {
    return $configureExit
  }

  return (Invoke-BuildStep -Context $Context)
}
