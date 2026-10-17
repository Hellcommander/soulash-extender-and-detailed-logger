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

# Lua is a build-time prerequisite fetched from lua.org (not stored in this repo).
$luaSource = Join-Path $repoRoot 'third_party\lua-5.4.9\src'
if (-not (Test-Path -LiteralPath (Join-Path $luaSource 'lua.h'))) {
    throw @"
Lua 5.4.9 sources are required to build the script sandbox but were not found.
  Official download page : https://www.lua.org/ftp/
  Tarball                : https://www.lua.org/ftp/lua-5.4.9.tar.gz
  Expected SHA-256       : 2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6
Run .\scripts\fetch-lua.ps1 (downloads and verifies it), or unpack the tarball so
that $luaSource\lua.h exists.
"@
}

# Only the interpreter core and the safe libraries. io, os, package (loadlib),
# debug and the standalone programs are deliberately never compiled.
$luaObjectDirectory = Join-Path $outputDirectory 'lua'
New-Item -ItemType Directory -Path $luaObjectDirectory -Force | Out-Null
$luaFiles = 'lapi','lauxlib','lbaselib','lcode','lctype','ldebug','ldo','ldump','lfunc','lgc',
    'llex','lmathlib','lmem','lobject','lopcodes','lparser','lstate','lstring','lstrlib',
    'ltable','ltablib','ltm','lundump','lutf8lib','lvm','lzio' | ForEach-Object { Join-Path $luaSource "$_.c" }
& cl.exe /nologo /TC /O2 /W3 /MT /c /I $luaSource "/Fo$luaObjectDirectory\" @luaFiles | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Compiling Lua failed.' }
$luaObjects = Get-ChildItem -LiteralPath $luaObjectDirectory -Filter *.obj | ForEach-Object { $_.FullName }

$sandboxObjectPath = Join-Path $outputDirectory 'script_sandbox.obj'
$conditionsObjectPath = Join-Path $outputDirectory 'conditions.obj'
& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /c /I $sourceDirectory `
    "/Fo$conditionsObjectPath" (Join-Path $sourceDirectory 'conditions.cpp')
if ($LASTEXITCODE -ne 0) { throw 'Compiling the conditions evaluator failed.' }
& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /c /I $luaSource /I $sourceDirectory `
    "/Fo$sandboxObjectPath" (Join-Path $sourceDirectory 'script_sandbox.cpp')
if ($LASTEXITCODE -ne 0) { throw 'Compiling the script sandbox failed.' }

& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /I $includeDirectory /I $sourceDirectory `
    "/Fo$objectDirectory" `
    (Join-Path $sourceDirectory 'manifest.cpp') `
    (Join-Path $sourceDirectory 'x64_length.cpp') `
    (Join-Path $sourceDirectory 'conditions.cpp') `
    (Join-Path $testDirectory 'tests.cpp') `
    $sandboxObjectPath @luaObjects `
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

$pluginDirectory = Join-Path $outputDirectory 'plugin-dir'
New-Item -ItemType Directory -Path $pluginDirectory -Force | Out-Null
$v3PluginPath = Join-Path $pluginDirectory 'Test.PluginA.dll'
& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /LD /I $includeDirectory `
    "/Fo$objectDirectory" `
    (Join-Path $testDirectory 'synthetic_v3_plugin.cpp') `
    /Fe:$v3PluginPath
if ($LASTEXITCODE -ne 0) { throw 'Building synthetic v3 plugin failed.' }
Copy-Item -LiteralPath $v3PluginPath -Destination (Join-Path $pluginDirectory 'Test.PluginB.dll') -Force
Copy-Item -LiteralPath $syntheticNativeModulePath -Destination (Join-Path $pluginDirectory 'Test.LegacyV2.dll') -Force
# Not a valid plugin id (space), must be skipped by discovery without failing.
Copy-Item -LiteralPath $v3PluginPath -Destination (Join-Path $pluginDirectory 'bad name.dll') -Force

$proxyPluginServicesObjectPath = Join-Path $outputDirectory 'proxy_plugin_services.obj'
$proxyX64ObjectPath = Join-Path $outputDirectory 'proxy_x64_length.obj'
$proxySha256ObjectPath = Join-Path $outputDirectory 'proxy_sha256.obj'
& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /c `
    "/Fo$proxySha256ObjectPath" (Join-Path $sourceDirectory 'sha256.cpp')
if ($LASTEXITCODE -ne 0) { throw 'Compiling shared SHA-256 failed.' }
& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /I $includeDirectory /c `
    "/Fo$proxyX64ObjectPath" (Join-Path $sourceDirectory 'x64_length.cpp')
if ($LASTEXITCODE -ne 0) { throw 'Compiling x64 length decoder failed.' }

& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /I $includeDirectory /c `
    "/Fo$proxyPluginServicesObjectPath" (Join-Path $sourceDirectory 'plugin_services.cpp')
if ($LASTEXITCODE -ne 0) { throw 'Compiling plugin services failed.' }

& cl.exe /nologo /std:c++17 /EHsc /W4 /WX /I $includeDirectory /c `
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
    $proxyModuleRuntimeObjectPath $proxyManifestObjectPath `
    $proxyPluginServicesObjectPath $proxyX64ObjectPath $proxySha256ObjectPath `
    $sandboxObjectPath $conditionsObjectPath @luaObjects bcrypt.lib
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
