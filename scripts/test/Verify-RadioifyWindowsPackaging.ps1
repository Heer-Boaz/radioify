Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
. (Join-Path $repoRoot "scripts\windows\RadioifyWindowsMsixBuild.ps1")

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}
function Write-Fixture {
    param([string]$Path, [string]$Text = "fixture")
    [IO.Directory]::CreateDirectory((Split-Path -Parent $Path)) | Out-Null
    [IO.File]::WriteAllText($Path, $Text)
}

$tempParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$root = Join-Path $tempParent ("radioify-packaging-test-" + [guid]::NewGuid().ToString("N"))
[IO.Directory]::CreateDirectory($root) | Out-Null
try {
    # Exercise the real process runner before replacing Windows SDK boundaries.
    # Enough output on both streams to exceed pipe buffers, plus an idle interval
    # which must produce progress before completion. Paths/arguments need quoting.
    $child = Join-Path $root "tool with spaces.ps1"
    Write-Fixture $child @'
param([string]$Echo, [int]$Result)
for ($i = 0; $i -lt 2500; ++$i) {
    [Console]::Out.WriteLine("payload $i " + ('x' * 80))
    [Console]::Error.WriteLine("diagnostic $i " + ('y' * 80))
}
Start-Sleep -Milliseconds 1200
[Console]::Out.WriteLine("echo=$Echo")
[Console]::Error.WriteLine("final-diagnostic")
exit $Result
'@
    $log = Join-Path $root "tool.log"
    $quoted = 'words with "quotes" and a trailing slash\'
    $powershell = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
    $messages = @(Invoke-RadioifyNativeTool -FilePath $powershell `
        -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $child, "-Echo", $quoted, "-Result", "0") `
        -LogPath $log -Activity "Fixture tool" -ProgressIntervalSeconds 1 6>&1)
    $console = $messages | Out-String
    $logged = Get-Content -LiteralPath $log -Raw
    Assert-True ($console -match 'elapsed' -and $console -notmatch 'payload 2499') "Tool progress is missing or payload output flooded the console."
    Assert-True ($logged.Contains("echo=$quoted") -and $logged.Contains('final-diagnostic')) "Native arguments or redirected output were lost."
    $failed = $false
    try {
        Invoke-RadioifyNativeTool -FilePath $powershell `
            -Arguments @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $child, "-Result", "7") `
            -LogPath $log -Activity "Expected failure" 6>$null
    } catch { $failed = $_.Exception.Message -match 'exit code 7' }
    Assert-True $failed "A native tool failure was swallowed."

    $dist = Join-Path $root "dist"
    $integration = Join-Path $dist "win11-explorer-integration"
    $manifest = Join-Path $integration "Package.appxmanifest"
    Write-Fixture $manifest '<Package><Identity Name="Packaging.Fixture" Publisher="CN=Fixture" Version="1.0.0.0" /></Package>'
    Write-Fixture (Join-Path $integration "radioify_explorer.dll")
    foreach ($relative in @(
        "radioify.exe", "radioify_analysis_worker.exe", "llama-cpp-LICENSE.txt",
        "models\ggml-large-v3.bin", "models\audio_separation\bandit-v2-multi-mask-core-fp16.onnx",
        "models\audio_separation\BandIt-v2-MODEL-CARD", "onnxruntime.dll", "Microsoft.Windows.AI.MachineLearning.dll",
        "transcript-runtime\runtime.json", "transcript-runtime\worker.py", "transcript-runtime\python.exe",
        "transcript-runtime\python313.dll", "transcript-runtime\msvcp140.dll", "transcript-runtime\python313._pth",
        "transcript-runtime\Lib\site-packages\torch\__init__.py",
        "transcript-runtime\models\Qwen3-ASR-1.7B-hf\model.safetensors", "transcript-runtime\models\large-v3.pt",
        "transcript-runtime\optional.txt", "transcript-runtime\__pycache__\ignored.pyc"
    )) { Write-Fixture (Join-Path $dist $relative) }
    foreach ($name in Get-RadioifyTranscriptBootstrapFiles) { Write-Fixture (Join-Path $dist "transcript\$name") }
    Write-Fixture (Join-Path $dist "transcript\native\msvcp140.dll")
    Write-Fixture (Join-Path $dist "transcript\native\optional.dll")

    # Cache identity must not depend on the caller's language or file order.
    $fingerprintFiles = [ordered]@{}
    $fingerprintDigests = @{}
    foreach ($name in @('z.txt', ([string][char]0x00e4 + '.txt'), 'a_file.txt', 'a.file.txt')) {
        $fingerprintFiles[$name] = Get-Item -LiteralPath (Join-Path $dist 'radioify.exe')
        $fingerprintDigests[$name] = Get-RadioifyFileDigest $fingerprintFiles[$name].FullName
    }
    $callerCulture = [Threading.Thread]::CurrentThread.CurrentCulture
    try {
        [Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::GetCultureInfo('en-US')
        $englishFingerprint = Get-RadioifyMsixInputFingerprint -Files $fingerprintFiles `
            -Digests $fingerprintDigests -ManifestPath $manifest -Thumbprint 'fixture-signer'
        $reversedFiles = [ordered]@{}
        $names = @($fingerprintFiles.Keys)
        [Array]::Reverse($names)
        foreach ($name in $names) { $reversedFiles[$name] = $fingerprintFiles[$name] }
        [Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::GetCultureInfo('sv-SE')
        $swedishFingerprint = Get-RadioifyMsixInputFingerprint -Files $reversedFiles `
            -Digests $fingerprintDigests -ManifestPath $manifest -Thumbprint 'fixture-signer'
        Assert-True ($englishFingerprint -eq $swedishFingerprint) "Package fingerprint depends on culture or input order."
    } finally { [Threading.Thread]::CurrentThread.CurrentCulture = $callerCulture }

    $script:packCount = 0
    $script:failSigning = $false
    $script:thumbprint = "fixture-signer"
    function Resolve-RadioifyWindowsSdkExecutable { param($ToolName) return $ToolName }
    function Ensure-RadioifyMsixSigningCertificate {
        param($Subject, $IntegrationDir)
        $cert = Join-Path (Split-Path -Parent $IntegrationDir) "Packaging.Fixture.cer"
        Write-Fixture $cert
        return [pscustomobject]@{ Certificate = [pscustomobject]@{ Thumbprint = $script:thumbprint }; CertificatePath = $cert }
    }
    function Invoke-RadioifyNativeTool {
        param($FilePath, $Arguments, $Activity, $LogPath)
        if ($FilePath -eq "makeappx.exe") {
            ++$script:packCount
            $script:uncompressed = $Arguments -contains "/nc"
            $packagePath = $Arguments[[Array]::IndexOf($Arguments, "/p") + 1]
            Write-Fixture $packagePath "package $script:packCount"
        } elseif ($script:failSigning) { throw "Fixture signing failed" }
    }
    function Assert-RadioifyMsixPackageSigned { param($Path, $ExpectedThumbprint) Wait-RadioifyMsixPackageFile $Path }

    Write-Fixture (Join-Path $integration "package-layout\radioify_analysis_worker.exe")
    Write-Fixture (Join-Path $integration "package-layout\models\audio_separation\bandit-v2-multi-mask-core-fp16.onnx")
    $first = New-RadioifySignedMsixPackage -IntegrationDir $integration -NoCompression
    Assert-True (-not $first.Reused -and $script:uncompressed) "Explicit uncompressed packaging was ignored."
    Assert-True (-not (Test-Path (Join-Path $first.LayoutPath "transcript-runtime")) -and
        -not (Test-Path (Join-Path $first.LayoutPath "models\ggml-large-v3.bin")) -and
        -not (Test-Path (Join-Path $first.LayoutPath "models\audio_separation\bandit-v2-multi-mask-core-fp16.onnx")) -and
        -not (Test-Path (Join-Path $first.LayoutPath "radioify_analysis_worker.exe"))) "Optional runtime, models or retired worker leaked into the installer."
    $runtime = Join-Path $first.LayoutPath "onnxruntime.dll"
    $creation = (Get-Item -LiteralPath $runtime).CreationTimeUtc
    # The native build republishes its template manifest before each package call.
    Set-RadioifyMsixManifestIdentityVersion -ManifestPath $manifest -Version "1.0.0.0"
    $second = New-RadioifySignedMsixPackage -IntegrationDir $integration -NoCompression
    Assert-True ($second.Reused -and $script:packCount -eq 1 -and $second.PackageVersion -eq $first.PackageVersion) "Identical build inputs were repackaged."
    Assert-True ((Get-RadioifyMsixManifestInfo $integration).Version -eq $first.PackageVersion) "Cached package version was not restored."

    Write-Fixture (Join-Path $dist "transcript\worker.py") "changed worker contents"
    Write-Fixture (Join-Path $first.LayoutPath "obsolete.dll")
    $changed = New-RadioifySignedMsixPackage -IntegrationDir $integration -NoCompression
    Assert-True (-not $changed.Reused -and $script:packCount -eq 2) "A changed payload did not invalidate the package."
    Assert-True ([version]$changed.PackageVersion -gt [version]$first.PackageVersion) "Changed payload kept the same default package version."
    Assert-True ((Get-Item -LiteralPath $runtime).CreationTimeUtc -eq $creation) "Unchanged runtime staging was recreated."
    Assert-True (-not (Test-Path (Join-Path $changed.LayoutPath "obsolete.dll"))) "Obsolete payload leaked into the package."
    Assert-True (-not (Test-Path (Join-Path $changed.LayoutPath "transcript-runtime\__pycache__\ignored.pyc"))) "Python cache leaked into the package."

    Remove-Item -LiteralPath (Join-Path $dist "transcript\native\optional.dll")
    $removed = New-RadioifySignedMsixPackage -IntegrationDir $integration -NoCompression
    Assert-True (-not $removed.Reused -and -not (Test-Path (Join-Path $removed.LayoutPath "transcript\native\optional.dll"))) "Removed input was still packaged."
    $explicit = New-RadioifySignedMsixPackage -IntegrationDir $integration -NoCompression -PackageVersion "2.3.4.5"
    Assert-True (-not $explicit.Reused -and $explicit.PackageVersion -eq "2.3.4.5") "Explicit version did not invalidate the package."
    $compressed = New-RadioifySignedMsixPackage -IntegrationDir $integration -PackageVersion "2.3.4.5"
    Assert-True (-not $compressed.Reused -and -not $script:uncompressed) "Compression setting was ignored by the cache."
    $script:thumbprint = "another-fixture-signer"
    $resigned = New-RadioifySignedMsixPackage -IntegrationDir $integration -PackageVersion "2.3.4.5"
    Assert-True (-not $resigned.Reused) "Changed signing certificate did not invalidate the package."
    Write-Fixture $resigned.PackagePath "broken"
    $repaired = New-RadioifySignedMsixPackage -IntegrationDir $integration -PackageVersion "2.3.4.5"
    Assert-True (-not $repaired.Reused) "A damaged package was reused."

    $previous = Get-Content -LiteralPath $repaired.PackagePath -Raw
    $previousManifestVersion = (Get-RadioifyMsixManifestInfo $integration).Version
    $previousState = Get-Content -LiteralPath (Join-Path $integration "package-state.json") -Raw
    $script:failSigning = $true
    Write-Fixture (Join-Path $dist "transcript\worker.py") "another changed worker"
    $failed = $false
    try { New-RadioifySignedMsixPackage -IntegrationDir $integration -NoCompression | Out-Null }
    catch { $failed = $_.Exception.Message -match 'Fixture signing failed' }
    Assert-True $failed "A signing failure was swallowed."
    Assert-True ((Get-Content -LiteralPath $repaired.PackagePath -Raw) -eq $previous) "A failed build destroyed the previous signed package."
    Assert-True ((Get-RadioifyMsixManifestInfo $integration).Version -eq $previousManifestVersion) "A failed build changed the published manifest version."
    Assert-True ((Get-Content -LiteralPath (Join-Path $integration "package-state.json") -Raw) -eq $previousState) "A failed build destroyed the last valid cache record."
    Assert-True (-not (Test-Path -LiteralPath "$($repaired.PackagePath).building.msix")) "A failed build left a partial package behind."
    $script:failSigning = $false
    $recovered = New-RadioifySignedMsixPackage -IntegrationDir $integration -NoCompression
    Assert-True (-not $recovered.Reused) "An unsuccessfully signed package was reused."
    Write-Fixture (Join-Path $integration "package-state.json") "incomplete json"
    $recovered = New-RadioifySignedMsixPackage -IntegrationDir $integration -NoCompression
    Assert-True (-not $recovered.Reused) "An incomplete cache record was trusted."
    Write-Fixture (Join-Path $integration "package-state.json") '{"Format":1,"InputFingerprint":"stale","PackageVersion":"invalid"}'
    $recovered = New-RadioifySignedMsixPackage -IntegrationDir $integration -NoCompression
    Assert-True (-not $recovered.Reused) "Malformed cache version blocked rebuilding."

    $payloadPath = Join-Path $dist "radioify.exe"
    $payloadFile = Get-Item -LiteralPath $payloadPath
    $payloadFile.LastWriteTimeUtc = $payloadFile.LastWriteTimeUtc.AddMinutes(1)
    $touched = New-RadioifySignedMsixPackage -IntegrationDir $integration -NoCompression
    Assert-True $touched.Reused "A timestamp-only change repackaged identical content."
    $payloadTime = (Get-Item -LiteralPath $payloadPath).LastWriteTimeUtc
    [IO.File]::WriteAllText($payloadPath, ('x' * $payloadFile.Length))
    (Get-Item -LiteralPath $payloadPath).LastWriteTimeUtc = $payloadTime
    $sameMetadata = New-RadioifySignedMsixPackage -IntegrationDir $integration -NoCompression
    Assert-True (-not $sameMetadata.Reused) "A changed payload with the same size and timestamp was missed."
    Assert-True ((Get-RadioifyFileDigest $payloadPath) -eq
        (Get-RadioifyFileDigest (Join-Path $sameMetadata.LayoutPath "radioify.exe"))) "Staging kept old bytes with matching metadata."

    $stagedPath = Join-Path $sameMetadata.LayoutPath "radioify.exe"
    $stagedTime = (Get-Item -LiteralPath $stagedPath).LastWriteTimeUtc
    [IO.File]::WriteAllText($stagedPath, ('y' * $payloadFile.Length))
    (Get-Item -LiteralPath $stagedPath).LastWriteTimeUtc = $stagedTime
    Write-Fixture (Join-Path $dist "transcript\worker.py") "force restaging"
    $restaged = New-RadioifySignedMsixPackage -IntegrationDir $integration -NoCompression
    Assert-True ((Get-RadioifyFileDigest $payloadPath) -eq (Get-RadioifyFileDigest $stagedPath)) "Damaged staging was repackaged."

    $cachedPackage = Get-Item -LiteralPath $restaged.PackagePath
    $cachedTime = $cachedPackage.LastWriteTimeUtc
    [IO.File]::WriteAllText($cachedPackage.FullName, ('z' * $cachedPackage.Length))
    (Get-Item -LiteralPath $cachedPackage.FullName).LastWriteTimeUtc = $cachedTime
    $repaired = New-RadioifySignedMsixPackage -IntegrationDir $integration -NoCompression
    Assert-True (-not $repaired.Reused) "Same-size package corruption with a preserved timestamp was missed."

    $held = Enter-RadioifyPackageLock -Directory $root -Name "fixture.lock"
    try {
        $blocked = $false
        try { Enter-RadioifyPackageLock -Directory $root -Name "fixture.lock" -TimeoutSeconds 0 | Out-Null }
        catch { $blocked = $_.Exception.Message -match 'Timed out waiting' }
        Assert-True $blocked "The packaging lock allowed a second owner."
    } finally { $held.Dispose() }
    $released = Enter-RadioifyPackageLock -Directory $root -Name "fixture.lock" -TimeoutSeconds 0
    $released.Dispose()
    Remove-Item -LiteralPath (Join-Path $dist "transcript\setup_runtime.py")
    $failed = $false
    try { New-RadioifySignedMsixPackage -IntegrationDir $integration -NoCompression | Out-Null }
    catch { $failed = $_.Exception.Message -match 'does not exist|Cannot find path' }
    Assert-True $failed "Missing bootstrap was hidden by a cache hit."

    # Run the real entry script in a tiny fixture repository. Neither a Windows
    # installation nor a distribution ZIP may be produced by this test.
    $fixtureRepo = Join-Path $root "repo"
    [IO.Directory]::CreateDirectory($fixtureRepo) | Out-Null
    $entry = Join-Path $fixtureRepo "install_radioify.ps1"
    Copy-Item -LiteralPath (Join-Path $repoRoot "install_radioify.ps1") -Destination $entry
    Write-Fixture (Join-Path $fixtureRepo "build.ps1") @'
param([switch]$Static, [switch]$Win11ExplorerIntegration, [switch]$Rebuild, [switch]$InstallDeps)
if (-not $Static -or -not $Win11ExplorerIntegration) { throw "Incorrect local build flags" }
Set-Content -LiteralPath (Join-Path $PSScriptRoot "built.txt") -Value "built"
exit 0
'@
    Write-Fixture (Join-Path $fixtureRepo "package_windows.ps1") 'throw "Local install must not create a distribution"'
    Write-Fixture (Join-Path $fixtureRepo "scripts\windows\RadioifyWindowsExplorerIntegrationHost.ps1") @'
function Invoke-RadioifyWindowsExplorerIntegrationScript {
    param($ScriptPath, $Operation, $Parameters, $UserCommandHint)
    if ($Operation -ne "install" -or -not $Parameters.ReplaceExisting) { throw "Incorrect install parameters" }
    $Parameters.IntegrationDir | Set-Content -LiteralPath (Join-Path $PSScriptRoot "installed.txt")
}
'@
    & $entry -Confirm:$false
    $installedMarker = Join-Path $fixtureRepo "scripts\windows\installed.txt"
    Assert-True ((Get-Content -LiteralPath $installedMarker).Trim() -eq (Join-Path $fixtureRepo "dist\win11-explorer-integration")) "Local install used the distribution bundle."
    Remove-Item -LiteralPath $installedMarker, (Join-Path $fixtureRepo "built.txt")
    & $entry -WhatIf
    Assert-True (-not (Test-Path $installedMarker) -and -not (Test-Path (Join-Path $fixtureRepo "built.txt"))) "WhatIf performed a build or install."
    Write-Fixture (Join-Path $fixtureRepo "build.ps1") 'exit 9'
    $failed = $false
    try { & $entry -Confirm:$false } catch { $failed = $_.Exception.Message -match 'Build failed with exit code 9' }
    Assert-True ($failed -and -not (Test-Path $installedMarker)) "Failed local build proceeded to installation."
    Write-Host "Windows packaging regression checks passed."
} finally {
    $stage = Get-Item -LiteralPath $root -Force -ErrorAction SilentlyContinue
    if ($stage) {
        if ((Split-Path -Parent $stage.FullName) -ne $tempParent -or
            ($stage.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to clean a packaging fixture outside its expected parent."
        }
        Remove-Item -LiteralPath $stage.FullName -Recurse -Force
    }
}
