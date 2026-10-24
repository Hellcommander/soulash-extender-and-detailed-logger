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

$oracleListing = Join-Path $outputDirectory 'native-tests.asm'
& dumpbin.exe /nologo /disasm:nobytes $testsPath | Set-Content -LiteralPath $oracleListing -Encoding ascii
if ($LASTEXITCODE -ne 0) { throw 'dumpbin disassembly failed.' }
& $testsPath --decoder-oracle $testsPath $oracleListing
if ($LASTEXITCODE -ne 0) { throw 'x64 length decoder oracle test failed.' }

$python = Get-Command python -ErrorAction SilentlyContinue
if ($python) {
    & $python.Source -I (Join-Path $repoRoot 'tools\validate_conditions.py') --self-test
    if ($LASTEXITCODE -ne 0) { throw 'Skill-conditions validator self-test failed.' }
    & $python.Source -I (Join-Path $repoRoot 'tools\validate_conditions.py') (Join-Path $repoRoot 'docs\examples\runic-magic-conditions.json') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Skill-conditions example does not validate.' }
    & $python.Source -I (Join-Path $repoRoot 'tools\effects_compile.py') --self-test
    if ($LASTEXITCODE -ne 0) { throw 'Effects compiler self-test failed.' }
    $effectsEmitDirectory = Join-Path $outputDirectory ("effects-emit-" + [guid]::NewGuid().ToString('N'))
    & $python.Source -I (Join-Path $repoRoot 'tools\effects_compile.py') (Join-Path $repoRoot 'docs\examples\runic-magic-effects.json') --emit $effectsEmitDirectory | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Effects example does not compile.' }
    foreach ($expected in 'abilities\arendeth_runic_magic_Entropy_Surge__1_1.json', 'generated_passives.json', 'generated_milestones.json', 'generated_conditions.json', 'effects_report.json') {
        if (-not (Test-Path -LiteralPath (Join-Path $effectsEmitDirectory $expected))) { throw "Effects compiler did not emit $expected." }
    }
}
else {
    Write-Output 'SKIP: python not found; skill-conditions validator not run.'
}

& $testsPath --conditions (Join-Path $repoRoot 'docs\examples\runic-magic-conditions.json')
if ($LASTEXITCODE -ne 0) { throw 'Skill-conditions evaluator test failed.' }

& $testsPath --effects (Join-Path $repoRoot 'docs\examples\runic-magic-effects.json')
if ($LASTEXITCODE -ne 0) { throw 'Effects loader/resolver test failed.' }

& $testsPath --sandbox
if ($LASTEXITCODE -ne 0) { throw 'Lua sandbox test failed.' }

$modsDirectoryPath = Join-Path $outputDirectory ("mods-" + [guid]::NewGuid().ToString('N'))
$exampleConditions = Join-Path $repoRoot 'docs\examples\runic-magic-conditions.json'
foreach ($mod in 'ModA', 'ModB', 'ModC', 'ModD') { New-Item -ItemType Directory -Path (Join-Path $modsDirectoryPath $mod) -Force | Out-Null }
New-Item -ItemType Directory -Path (Join-Path $modsDirectoryPath 'ModA\extender') -Force | Out-Null
Copy-Item -LiteralPath $exampleConditions -Destination (Join-Path $modsDirectoryPath 'ModA\extender\conditions.json')
Copy-Item -LiteralPath (Join-Path $repoRoot 'docs\examples\runic-magic-effects.json') -Destination (Join-Path $modsDirectoryPath 'ModA\extender\effects.json')
New-Item -ItemType Directory -Path (Join-Path $modsDirectoryPath 'ModB\extender') -Force | Out-Null
$oneCondition = '{"id":"dup","target":{"kind":"passive","id":"%s"},"when":{"in_combat":true}}'
[System.IO.File]::WriteAllText((Join-Path $modsDirectoryPath 'ModB\extender\conditions.json'),
    ('{"spec":1,"conditions":[' + $oneCondition.Replace('%s', 'a') + ',' + $oneCondition.Replace('%s', 'b') + ']}'))
New-Item -ItemType Directory -Path (Join-Path $modsDirectoryPath 'ModD\extender') -Force | Out-Null
[System.IO.File]::WriteAllText((Join-Path $modsDirectoryPath 'ModD\extender\conditions.json'),
    ('{"spec":1,"conditions":[' + $oneCondition.Replace('%s', 'z') + ']}'))
[System.IO.File]::WriteAllText((Join-Path $modsDirectoryPath 'ModD\extender\effects.json'), '{"spec":2,"mod":"ModD"}')
$conditionsLogPath = Join-Path $outputDirectory ("conditions-" + [guid]::NewGuid().ToString('N') + '.log')
& $testsPath --conditions-discovery $proxyPath $fakeOriginalPath $modsDirectoryPath $conditionsLogPath
if ($LASTEXITCODE -ne 0) { throw 'Condition discovery test failed.' }

$scriptDirectoryPath = Join-Path $outputDirectory 'script-dir'
New-Item -ItemType Directory -Path $scriptDirectoryPath -Force | Out-Null
[System.IO.File]::WriteAllText((Join-Path $scriptDirectoryPath 'Test.Hello.lua'),
    "ext.log('top level')`nfunction on_load() ext.log('hello from on_load') end`n")
[System.IO.File]::WriteAllText((Join-Path $scriptDirectoryPath 'Test.Evil.lua'),
    "local f = io.open('C:\\Windows\\win.ini')`nos.execute('calc')`n")
[System.IO.File]::WriteAllText((Join-Path $scriptDirectoryPath 'Test.Loop.lua'), "while true do end`n")
[System.IO.File]::WriteAllText((Join-Path $scriptDirectoryPath 'Test.After.lua'), "after = true`n")
[System.IO.File]::WriteAllText((Join-Path $scriptDirectoryPath 'bad name.lua'), "ext.log('must not run')`n")
$scriptLogPath = Join-Path $outputDirectory ("scripts-" + [guid]::NewGuid().ToString('N') + '.log')
& $testsPath --scripts $proxyPath $fakeOriginalPath $scriptDirectoryPath $scriptLogPath
if ($LASTEXITCODE -ne 0) { throw 'Script loading test failed.' }

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

$fallbackMarkerBase = Join-Path $outputDirectory ("debugger-fallback-" + [guid]::NewGuid().ToString('N'))
& $testsPath --debugger-fallback $proxyPath $fakeOriginalPath $moduleManifestPath $fallbackMarkerBase
if ($LASTEXITCODE -ne 0) { throw 'Debugger fallback test failed.' }

$pluginDirectoryPath = Join-Path $outputDirectory 'plugin-dir'
$pluginMarkerPath = Join-Path $outputDirectory ("plugin-dir-" + [guid]::NewGuid().ToString('N') + '.txt')
try {
    & $testsPath --plugin-dir $proxyPath $fakeOriginalPath $pluginDirectoryPath $pluginMarkerPath
    if ($LASTEXITCODE -ne 0) { throw 'Plugin directory discovery test failed.' }
}
finally {
    if (Test-Path -LiteralPath $pluginMarkerPath) {
        Remove-Item -LiteralPath $pluginMarkerPath -Force
    }
}

$allowlistIniPath = Join-Path $outputDirectory 'SoulashExtender.ini'
$allowlistMarkerPath = Join-Path $outputDirectory ("plugin-allowlist-" + [guid]::NewGuid().ToString('N') + '.txt')
$pluginAHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $pluginDirectoryPath 'Test.PluginA.dll')).Hash.ToUpperInvariant()
try {
    [System.IO.File]::WriteAllText(
        $allowlistIniPath,
        "[Plugins]`r`nRequireHash=1`r`n[PluginHashes]`r`nTest.PluginA=$pluginAHash`r`nTest.LegacyV2=$('0' * 64)`r`n",
        [System.Text.Encoding]::ASCII)
    & $testsPath --plugin-allowlist $proxyPath $fakeOriginalPath $pluginDirectoryPath $allowlistMarkerPath
    if ($LASTEXITCODE -ne 0) { throw 'Plugin allowlist test failed.' }
}
finally {
    foreach ($leftover in @($allowlistIniPath, $allowlistMarkerPath)) {
        if (Test-Path -LiteralPath $leftover) { Remove-Item -LiteralPath $leftover -Force }
    }
}

$extenderHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $syntheticExtenderPath).Hash.ToLowerInvariant()
& $testsPath --chain $proxyPath $fakeOriginalPath $syntheticExtenderPath $extenderHash default
if ($LASTEXITCODE -ne 0) { throw 'Chain loading test failed.' }

$chainIniPath = Join-Path $outputDirectory 'SoulashExtender.ini'
try {
    [System.IO.File]::WriteAllText(
        $chainIniPath,
        "[Plugins]`r`nRequireHash=1`r`n",
        [System.Text.Encoding]::ASCII)
    & $testsPath --chain $proxyPath $fakeOriginalPath $syntheticExtenderPath $extenderHash require-hash
    if ($LASTEXITCODE -ne 0) { throw 'Chain loading RequireHash test failed.' }
}
finally {
    if (Test-Path -LiteralPath $chainIniPath) { Remove-Item -LiteralPath $chainIniPath -Force }
}

$distDirectory = Join-Path $outputDirectory ("dist-" + [guid]::NewGuid().ToString('N'))
& (Join-Path $PSScriptRoot 'package.ps1') -OutputDirectory $distDirectory | Out-Null
foreach ($expected in 'BugSplat64.dll', 'SoulashExtender.ini', 'INSTALL.txt', 'SHA256SUMS.txt', 'THIRD_PARTY_NOTICES.txt', 'check-install.ps1', 'SoulashExtender\Plugins', 'SoulashExtender\Scripts') {
    if (-not (Test-Path -LiteralPath (Join-Path $distDirectory $expected))) { throw "Package is missing $expected." }
}
$packagedIni = Get-Content -LiteralPath (Join-Path $distDirectory 'SoulashExtender.ini') -Raw
if ($packagedIni -notmatch '(?m)^\[Plugins\]\s*\r?\nEnabled=1' -or $packagedIni -notmatch '(?m)^\[Scripts\]\s*\r?\nEnabled=1') { throw 'Packaged INI does not enable plugins and scripts.' }
if ((Get-Content -LiteralPath (Join-Path $distDirectory 'THIRD_PARTY_NOTICES.txt') -Raw) -notmatch 'Permission is hereby granted') { throw 'Packaged notices are missing the Lua licence text.' }
$sumLine = (Get-Content -LiteralPath (Join-Path $distDirectory 'SHA256SUMS.txt')) | Where-Object { $_ -like '*BugSplat64.dll' }
if ($sumLine -ne ('{0} *BugSplat64.dll' -f (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $distDirectory 'BugSplat64.dll')).Hash.ToLowerInvariant())) { throw 'SHA256SUMS does not match the packaged DLL.' }
Write-Output 'PASS: packaged folder is complete and its INI enables plugins and scripts'

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
