<#
.SYNOPSIS
  Assembles the files to copy into the Soulash 2 folder from the build output.
  Installs nothing and never touches the game folder.

.DESCRIPTION
  Produces BugSplat64.dll (the proxy), a SoulashExtender.ini template,
  empty SoulashExtender\Plugins and SoulashExtender\Scripts folders,
  THIRD_PARTY_NOTICES.txt (Lua's MIT notice, required for binaries),
  check-install.ps1, INSTALL.txt and SHA256SUMS.txt. Build first
  (scripts\build.ps1), or let this script do it.

.EXAMPLE
  .\scripts\package.ps1 -OutputDirectory D:\SoulashExtenderPackage
#>
param(
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $repoRoot '.build'
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $buildDirectory 'dist' }

$proxySource = Join-Path $buildDirectory 'BugSplat64.dll'
if (-not (Test-Path -LiteralPath $proxySource)) {
    # -GameDir '' : build only; packaging never deploys to or edits the game folder.
    & (Join-Path $PSScriptRoot 'build.ps1') -GameDir ''
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
foreach ($name in 'SoulashExtender\Plugins', 'SoulashExtender\Scripts') {
    New-Item -ItemType Directory -Path (Join-Path $OutputDirectory $name) -Force | Out-Null
}

$dllPath = Join-Path $OutputDirectory 'BugSplat64.dll'
Copy-Item -LiteralPath $proxySource -Destination $dllPath -Force
foreach ($tool in 'check-install.ps1', 'plugin-hashes.ps1', 'plugin-pins.ps1', 'PluginHashes.cmd') {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $tool) -Destination (Join-Path $OutputDirectory $tool) -Force
}
$dllHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $dllPath).Hash.ToLowerInvariant()

# The DLL contains Lua; its MIT notice must accompany binaries. The text is
# taken from the Lua sources the build used.
$luaHeader = Join-Path $repoRoot 'third_party\lua-5.4.9\src\lua.h'
if (-not (Test-Path -LiteralPath $luaHeader)) {
    throw 'Lua sources not found; run scripts\fetch-lua.ps1 so the licence notice can be packaged.'
}
$lines = Get-Content -LiteralPath $luaHeader
$start = ($lines | Select-String -Pattern '^\* Copyright \(C\) 1994').LineNumber
$block = @()
for ($index = $start - 1; $index -lt $lines.Count; $index++) {
    if ($lines[$index] -match '^\*{20,}') { break }
    $block += ($lines[$index] -replace '^\*\s?', '')
}
$notice = @('Third-party software included in BugSplat64.dll (the extender proxy)', '', 'Lua 5.4.9 (https://www.lua.org/)', '') + $block
[System.IO.File]::WriteAllLines((Join-Path $OutputDirectory 'THIRD_PARTY_NOTICES.txt'), $notice, [System.Text.Encoding]::ASCII)

# An existing INI is kept (it may hold the user's plugin pins).
$iniPath = Join-Path $OutputDirectory 'SoulashExtender.ini'
if (-not (Test-Path -LiteralPath $iniPath)) {
    $ini = @(
        '[Diagnostics]',
        'Enabled=1',
        'LogPath=logs\proxy-crash.log',
        '',
        '[Plugins]',
        'Enabled=1',
        'Directory=SoulashExtender\Plugins',
        '; Optional: set RequireHash=1 and list each plugin below to load only pinned DLLs.',
        '; PluginHashes.cmd (in the game folder) finds the hashes and can write them in.',
        '; RequireHash=1',
        '',
        '[PluginHashes]',
        '; MyPlugin=<64 hex characters, SHA-256 of SoulashExtender\Plugins\MyPlugin.dll>',
        '',
        '[Scripts]',
        'Enabled=1',
        'Directory=SoulashExtender\Scripts',
        '',
        '[Conditions]',
        'Enabled=1',
        '; Directory defaults to <game folder>\data\mods',
        '',
        '[UpdateGuard]',
        '; Detects game updates, writes SoulashExtender\compat-report.txt, and skips native',
        '; plugins after repeated crashes (docs/update-guard.md). Defaults shown.',
        'Enabled=1',
        'SafeModeAfterUncleanExits=2',
        'ForceSafeMode=0'
    )
    [System.IO.File]::WriteAllLines($iniPath, $ini, [System.Text.Encoding]::ASCII)
}

$install = @(
    'Soulash 2 extender - install notes',
    '',
    'Nothing here has been installed. These files are copied into the game folder.',
    '',
    '  1. Close the game. Open the game folder (Steam > Soulash 2 > Manage > Browse local files).',
    '  2. Rename the game''s BugSplat64.dll to BugSplat64_original.dll.',
    '     Never overwrite an existing BugSplat64_original.dll; stop and sort that out first.',
    '  3. Copy these into the game folder:',
    '       BugSplat64.dll, SoulashExtender.ini, SoulashExtender\ (and check-install.ps1 if wanted).',
    '     For the optional hash pinning also copy PluginHashes.cmd, plugin-hashes.ps1 and plugin-pins.ps1.',
    '  4. Plugin DLLs go in SoulashExtender\Plugins, sandboxed *.lua scripts in',
    '     SoulashExtender\Scripts. Diagnostics go to logs\proxy-crash.log.',
    '',
    'EVERY GAME UPDATE OR "VERIFY INTEGRITY" RESTORES THE VENDOR BugSplat64.dll AND REMOVES THE EXTENDER.',
    'After each update run:  check-install.ps1 -GameDir "<game folder>"  and repeat steps 2-3.',
    '',
    'Optional and UNTESTED: set the proxy read-only (attrib +R BugSplat64.dll) to make updates less likely',
    'to replace it. Steam may overwrite it anyway or fail the update; if so, run attrib -R and retry.',
    '',
    'Rollback (game closed): delete the proxy BugSplat64.dll, rename BugSplat64_original.dll',
    'back to BugSplat64.dll, and verify its signature.',
    '',
    'Both plugins and the proxy run with full process privileges. Optionally pin plugins in the',
    'INI ([Plugins] RequireHash=1 and [PluginHashes]): double-click PluginHashes.cmd in the game',
    'folder; it lists each plugin''s SHA-256 and asks before writing the pins (and RequireHash=1)',
    'into SoulashExtender.ini. Run it again after adding or updating a plugin.',
    'Scripts are sandboxed but the sandbox',
    'lowers risk, it does not remove it. Tested against synthetic targets only.',
    '',
    "BugSplat64.dll (proxy) SHA-256: $dllHash"
)
[System.IO.File]::WriteAllLines((Join-Path $OutputDirectory 'INSTALL.txt'), $install, [System.Text.Encoding]::ASCII)

$sums = foreach ($name in 'BugSplat64.dll', 'check-install.ps1', 'plugin-hashes.ps1', 'plugin-pins.ps1', 'PluginHashes.cmd',
        'SoulashExtender.ini', 'INSTALL.txt', 'THIRD_PARTY_NOTICES.txt') {
    $file = Join-Path $OutputDirectory $name
    '{0} *{1}' -f (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLowerInvariant(), $name
}
[System.IO.File]::WriteAllLines((Join-Path $OutputDirectory 'SHA256SUMS.txt'), $sums, [System.Text.Encoding]::ASCII)

Write-Output "Packaged to $OutputDirectory"
