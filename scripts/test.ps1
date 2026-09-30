$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$outputDirectory = Join-Path $repoRoot '.build'
& (Join-Path $PSScriptRoot 'build.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }

$testsPath = Join-Path $outputDirectory 'native-tests.exe'
$proxyPath = Join-Path $outputDirectory 'BugSplat64.dll'
$fakeOriginalPath = Join-Path $outputDirectory 'BugSplat64_original.dll'
$syntheticExtenderPath = Join-Path $outputDirectory 'SyntheticExtender.dll'
$diagnosticsPath = Join-Path $outputDirectory 'SoulashDiagnostics.dll'
$hostPath = Join-Path $outputDirectory 'native-module-host.exe'
$syntheticModulePath = Join-Path $outputDirectory 'synthetic-modules\native\SyntheticNativeModule.dll'

& $testsPath --unit
if ($LASTEXITCODE -ne 0) { throw 'Manifest parser tests failed.' }

$disabledLogPath = Join-Path $outputDirectory ("diagnostics-disabled-" + [guid]::NewGuid().ToString('N') + '.log')
& $testsPath --proxy $proxyPath $fakeOriginalPath disabled $disabledLogPath
if ($LASTEXITCODE -ne 0) { throw 'Default-disabled proxy forwarding test failed.' }

$optInLogPath = Join-Path $outputDirectory ("diagnostics-opt-in-" + [guid]::NewGuid().ToString('N') + '.log')
try {
    & $testsPath --proxy $proxyPath $fakeOriginalPath enabled $optInLogPath
    if ($LASTEXITCODE -ne 0) { throw 'Opt-in diagnostics proxy forwarding test failed.' }
}
finally {
    if (Test-Path -LiteralPath $optInLogPath) {
        Remove-Item -LiteralPath $optInLogPath -Force
    }
}

$failedLogDirectory = Join-Path $outputDirectory ("diagnostics-fail-open-" + [guid]::NewGuid().ToString('N'))
$failedLogPath = Join-Path $failedLogDirectory 'crash.log'
& $testsPath --proxy $proxyPath $fakeOriginalPath failure $failedLogPath
if ($LASTEXITCODE -ne 0) { throw 'Logger-failure-isolation proxy forwarding test failed.' }

$iniLogPath = Join-Path $outputDirectory ("diagnostics-ini-" + [guid]::NewGuid().ToString('N') + '.log')
$iniPath = Join-Path $outputDirectory 'SoulashExtender.ini'
try {
    [System.IO.File]::WriteAllText(
        $iniPath,
        "[Diagnostics]`r`nEnabled=1`r`nLogPath=$iniLogPath`r`n",
        [System.Text.Encoding]::Unicode)
    & $testsPath --proxy $proxyPath $fakeOriginalPath ini $iniLogPath
    if ($LASTEXITCODE -ne 0) { throw 'INI-enabled proxy diagnostics test failed.' }
}
finally {
    if (Test-Path -LiteralPath $iniPath) {
        Remove-Item -LiteralPath $iniPath -Force
    }
    if (Test-Path -LiteralPath $iniLogPath) {
        Remove-Item -LiteralPath $iniLogPath -Force
    }
}

& $testsPath --coexist $proxyPath $fakeOriginalPath $syntheticExtenderPath
if ($LASTEXITCODE -ne 0) { throw 'Synthetic extender coexistence test failed.' }

$moduleManifestPath = Join-Path $outputDirectory 'synthetic-modules\native_modules.txt'
[System.IO.File]::WriteAllText(
    $moduleManifestPath,
    "SOULASH2_NATIVE_MODULES_V1`r`nid=Test.SyntheticNativeModule`r`nmodule=native/SyntheticNativeModule.dll`r`n",
    [System.Text.Encoding]::ASCII)
$moduleMarkerPath = Join-Path $outputDirectory ("module-loaded-" + [guid]::NewGuid().ToString('N') + '.txt')
& $testsPath --module $proxyPath $fakeOriginalPath $moduleManifestPath $moduleMarkerPath
if ($LASTEXITCODE -ne 0) { throw 'Opt-in synthetic native module test failed.' }

$invalidModuleManifestPath = Join-Path $outputDirectory ("missing-manifest-" + [guid]::NewGuid().ToString('N') + '.txt')
& $testsPath --module-failure $proxyPath $fakeOriginalPath $invalidModuleManifestPath
if ($LASTEXITCODE -ne 0) { throw 'Native module load failure-isolation test failed.' }

$missingDirectory = Join-Path $outputDirectory ("missing-original-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $missingDirectory -Force | Out-Null
$missingProxyPath = Join-Path $missingDirectory 'BugSplat64.dll'
$missingTestsPath = Join-Path $missingDirectory 'native-tests.exe'
Copy-Item -LiteralPath $proxyPath -Destination $missingProxyPath -Force
Copy-Item -LiteralPath $testsPath -Destination $missingTestsPath -Force
if (Test-Path -LiteralPath (Join-Path $missingDirectory 'BugSplat64_original.dll')) {
    throw 'Missing-original test directory unexpectedly contains the fake original.'
}
& $missingTestsPath --missing $missingProxyPath
if ($LASTEXITCODE -ne 0) { throw 'Missing-original test failed.' }

$syntheticRoot = Join-Path $outputDirectory 'synthetic-modules'
New-Item -ItemType Directory -Path $syntheticRoot -Force | Out-Null
$syntheticManifest = Join-Path $outputDirectory 'synthetic-native_modules.txt'
[System.IO.File]::WriteAllText(
    $syntheticManifest,
    "SOULASH2_NATIVE_MODULES_V1`r`nid=Test.Synthetic`r`nmodule=native/Synthetic.dll`r`n",
    [System.Text.Encoding]::ASCII)
& $hostPath --validate $syntheticManifest $syntheticRoot
if ($LASTEXITCODE -ne 0) { throw 'Synthetic manifest host validation failed.' }

$localLogPath = Join-Path ([System.IO.Path]::GetTempPath()) ("soulash-diagnostics-" + [guid]::NewGuid().ToString('N') + '.log')
try {
    & $testsPath --diagnostics $diagnosticsPath $localLogPath $syntheticModulePath
    if ($LASTEXITCODE -ne 0) { throw 'Diagnostics startup tests failed.' }
}
finally {
    if (Test-Path -LiteralPath $localLogPath) {
        Remove-Item -LiteralPath $localLogPath -Force
    }
}

Write-Output 'All native extender tests passed.'
