$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$outputDirectory = Join-Path $repoRoot '.build'
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null

foreach ($tool in @('cl.exe', 'link.exe', 'dumpbin.exe')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool was not found on PATH. Run this script from an x64 Visual Studio developer prompt."
    }
}

$includeDirectory = Join-Path $repoRoot 'include'
$sourceDirectory = Join-Path $repoRoot 'src'
$testDirectory = Join-Path $repoRoot 'tests'
$objectDirectory = $outputDirectory + '\'
$proxyPath = Join-Path $outputDirectory 'BugSplat64.dll'
$proxyImportLibraryPath = Join-Path $outputDirectory 'BugSplat64_proxy.lib'
$syntheticExtenderPath = Join-Path $outputDirectory 'SyntheticExtender.dll'
$proxyObjectPath = Join-Path $outputDirectory 'bugsplat_proxy.obj'
$proxyDiagnosticsObjectPath = Join-Path $outputDirectory 'proxy_diagnostics.obj'
$proxyModuleRuntimeObjectPath = Join-Path $outputDirectory 'proxy_module_runtime.obj'
$proxyManifestObjectPath = Join-Path $outputDirectory 'proxy_manifest.obj'
$fakeOriginalPath = Join-Path $outputDirectory 'BugSplat64_original.dll'
$diagnosticsPath = Join-Path $outputDirectory 'SoulashDiagnostics.dll'
$hostPath = Join-Path $outputDirectory 'native-module-host.exe'
$testsPath = Join-Path $outputDirectory 'native-tests.exe'
$syntheticModulesDirectory = Join-Path $outputDirectory 'synthetic-modules'
$syntheticModuleNativeDirectory = Join-Path $syntheticModulesDirectory 'native'
$syntheticNativeModulePath = Join-Path $syntheticModuleNativeDirectory 'SyntheticNativeModule.dll'
New-Item -ItemType Directory -Path $syntheticModuleNativeDirectory -Force | Out-Null

& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /I $includeDirectory `
    "/Fo$objectDirectory" `
    (Join-Path $sourceDirectory 'manifest.cpp') `
    (Join-Path $testDirectory 'tests.cpp') `
    /Fe:$testsPath
if ($LASTEXITCODE -ne 0) { throw 'Building native-tests.exe failed.' }

& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /I $includeDirectory `
    "/Fo$objectDirectory" `
    (Join-Path $sourceDirectory 'manifest.cpp') `
    (Join-Path $sourceDirectory 'native_module_host.cpp') `
    /Fe:$hostPath
if ($LASTEXITCODE -ne 0) { throw 'Building native-module-host.exe failed.' }

& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /LD /I $includeDirectory `
    "/Fo$objectDirectory" `
    (Join-Path $sourceDirectory 'diagnostics.cpp') `
    /Fe:$diagnosticsPath /link bcrypt.lib
if ($LASTEXITCODE -ne 0) { throw 'Building SoulashDiagnostics.dll failed.' }

& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /LD `
    "/Fo$objectDirectory" `
    (Join-Path $testDirectory 'fake_bugsplat.cpp') `
    /Fe:$fakeOriginalPath
if ($LASTEXITCODE -ne 0) { throw 'Building fake BugSplat DLL failed.' }

& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /LD /I $includeDirectory `
    "/Fo$objectDirectory" `
    (Join-Path $testDirectory 'synthetic_native_module.cpp') `
    /Fe:$syntheticNativeModulePath
if ($LASTEXITCODE -ne 0) { throw 'Building synthetic native module failed.' }

& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /c `
    "/Fo$proxyObjectPath" (Join-Path $sourceDirectory 'bugsplat_proxy.cpp')
if ($LASTEXITCODE -ne 0) { throw 'Compiling proxy thunks failed.' }

& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /c /DSOULASH_DIAGNOSTICS_EMBEDDED /I $includeDirectory `
    "/Fo$proxyDiagnosticsObjectPath" (Join-Path $sourceDirectory 'diagnostics.cpp')
if ($LASTEXITCODE -ne 0) { throw 'Compiling embedded diagnostics failed.' }

& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /I $includeDirectory /c `
    "/Fo$proxyModuleRuntimeObjectPath" (Join-Path $sourceDirectory 'module_runtime.cpp')
if ($LASTEXITCODE -ne 0) { throw 'Compiling opt-in module runtime failed.' }

& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /I $includeDirectory /c `
    "/Fo$proxyManifestObjectPath" (Join-Path $sourceDirectory 'manifest.cpp')
if ($LASTEXITCODE -ne 0) { throw 'Compiling proxy manifest parser failed.' }

& link.exe /nologo /dll /machine:x64 `
    "/def:$sourceDirectory\bugsplat_proxy.def" `
    "/implib:$proxyImportLibraryPath" "/out:$proxyPath" `
    $proxyObjectPath $proxyDiagnosticsObjectPath `
    $proxyModuleRuntimeObjectPath $proxyManifestObjectPath bcrypt.lib
if ($LASTEXITCODE -ne 0) { throw 'Building BugSplat64.dll proxy failed.' }

& link.exe /nologo /dll /noentry /machine:x64 /ignore:4001 `
    "/def:$testDirectory\synthetic_extender.def" `
    "/out:$syntheticExtenderPath"
if ($LASTEXITCODE -ne 0) { throw 'Building synthetic extender forwarder DLL failed.' }

$expectedExports = @(
    '?sendAdditionalFile@MiniDmpSender@@QEAAXPEB_W@Z',
    '??1MiniDmpSender@@UEAA@XZ',
    '??0MiniDmpSender@@QEAA@PEB_W000K@Z',
    '?setGuardByteBufferSize@MiniDmpSender@@QEAAHH@Z'
)
$proxyExports = & dumpbin.exe /nologo /exports $proxyPath
if ($LASTEXITCODE -ne 0) { throw 'Inspecting BugSplat64.dll exports failed.' }
foreach ($symbol in $expectedExports) {
    if (($proxyExports -join "`n") -notmatch [regex]::Escape($symbol)) {
        throw "Proxy is missing expected export $symbol."
    }
}
$exportRows = @($proxyExports | Where-Object { $_ -match '^\s+\d+\s+\d+\s+' })
if ($exportRows.Count -ne 4) {
    throw "Proxy must export exactly four symbols; found $($exportRows.Count)."
}

$proxyImports = & dumpbin.exe /nologo /imports $proxyPath
if ($LASTEXITCODE -ne 0) { throw 'Inspecting BugSplat64.dll imports failed.' }
if (($proxyExports -join "`n") -match '\(forwarded to') {
    throw 'Proxy exports must be ABI-preserving wrapper thunks, not linker forwarders.'
}
if (($proxyImports -join "`n") -match 'BugSplat64_original') {
    throw 'Proxy unexpectedly has a static import of BugSplat64_original.dll.'
}

$syntheticExtenderExports = & dumpbin.exe /nologo /exports $syntheticExtenderPath
if ($LASTEXITCODE -ne 0) { throw 'Inspecting synthetic extender exports failed.' }
foreach ($symbol in $expectedExports) {
    $forwarderPattern = (
        [regex]::Escape($symbol) +
        '\s+\(forwarded to\s+BugSplat64_original\.' +
        [regex]::Escape($symbol) +
        '\)'
    )
    if (($syntheticExtenderExports -join "`n") -notmatch $forwarderPattern) {
        throw "Synthetic extender is missing forwarder $symbol."
    }
}

Write-Output "Built x64 artifacts in $outputDirectory"
