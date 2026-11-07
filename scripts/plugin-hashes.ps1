<#
.SYNOPSIS
  Plugin hash finder: lists every plugin DLL with its SHA-256 and the
  [PluginHashes] lines for SoulashExtender.ini, and can write them in for you.

.DESCRIPTION
  Hash pinning is optional. With [Plugins] RequireHash=1 the extender loads a
  plugin only when [PluginHashes] lists <PluginId>=<SHA-256 of the DLL>, and
  every new or updated plugin DLL needs a fresh line. This script finds them.

  Without switches it only reports (nothing is changed):
    * each DLL in the plugin folder, its SHA-256, and whether its pin is new,
      matching, or out of date;
    * pins for plugins that are no longer there;
    * the ready-to-paste [PluginHashes] lines.

  -Write   adds or updates the pins in SoulashExtender.ini. Only the pins of
           the plugins found change; everything else in the file stays as it
           is, and the previous file is kept as SoulashExtender.ini.bak.
  -RequireHash  also sets [Plugins] RequireHash=1 (implies -Write).
  -Prompt  asks before writing (what PluginHashes.cmd uses).

  The game folder is -GameDir, else this script's own folder when it sits in
  the game folder, else %SOULASH2_GAME_DIR%, else the default Steam path.

.EXAMPLE
  .\plugin-hashes.ps1                               # report only
  .\plugin-hashes.ps1 -Write                        # pin what is installed now
  .\plugin-hashes.ps1 -RequireHash                  # pin it and turn checking on
  .\plugin-hashes.ps1 -GameDir 'E:\SteamLibrary\steamapps\common\Soulash 2'
#>
param(
    [string]$GameDir,
    [switch]$Write,
    [switch]$RequireHash,
    [switch]$Prompt
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'plugin-pins.ps1')

# .NET directly: works in Windows PowerShell 5.1 and PowerShell 7 alike.
function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($path)
    try { return ([System.BitConverter]::ToString($sha.ComputeHash($stream)) -replace '-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $sha.Dispose() }
}

# Values of one INI section (keys case-insensitive, comments skipped).
function Read-IniSection([string]$path, [string]$section) {
    $values = [ordered]@{}
    $current = $null
    foreach ($line in [System.IO.File]::ReadAllLines($path)) {
        $trimmed = $line.Trim()
        if ($trimmed -match '^\[\s*(.+?)\s*\]$') { $current = $Matches[1]; continue }
        if ($current -ieq $section -and $trimmed -match '^([^=;#][^=]*?)\s*=\s*(.*)$') {
            $values[$Matches[1]] = $Matches[2].Trim()
        }
    }
    return $values
}

# Same rule as the extender (module_runtime.cpp IsValidPluginId).
function Test-PluginId([string]$id) {
    return $id -cmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$'
}

# Rewrites lines of the INI text, keeping its newline style and BOM.
function Edit-IniText([string]$path, [scriptblock]$edit) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $hasBom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    $offset = if ($hasBom) { 3 } else { 0 }
    $text = [System.Text.Encoding]::UTF8.GetString($bytes, $offset, $bytes.Length - $offset)
    $newline = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
    $endsWithNewline = $text.EndsWith("`n")
    $lines = [System.Collections.Generic.List[string]]::new()
    foreach ($line in ($text -split "`r?`n")) { $lines.Add($line) }
    if ($endsWithNewline) { $lines.RemoveAt($lines.Count - 1) }
    & $edit $lines
    $output = [string]::Join($newline, $lines) + $(if ($endsWithNewline) { $newline } else { '' })
    $encoded = [System.Text.Encoding]::UTF8.GetBytes($output)
    if ($hasBom) { $encoded = [byte[]](0xEF, 0xBB, 0xBF) + $encoded }
    [System.IO.File]::WriteAllBytes($path, $encoded)
}

function Find-Section($lines, [string]$name) {
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -match ('^\s*\[\s*' + [regex]::Escape($name) + '\s*\]\s*$')) { return $i }
    }
    return -1
}

function Find-SectionEnd($lines, [int]$start) {
    for ($i = $start + 1; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -match '^\s*\[') { return $i }
    }
    return $lines.Count
}

function Read-YesNo([string]$question) {
    while ($true) {
        $answer = (Read-Host "$question [y/n]").Trim()
        if ($answer -match '^(y|yes)$') { return $true }
        if ($answer -match '^(n|no)$') { return $false }
    }
}

# --- Locate the game folder -------------------------------------------------
$candidates = @()
if ($GameDir) { $candidates = @($GameDir) }
else {
    $candidates = @($PSScriptRoot, $env:SOULASH2_GAME_DIR, 'D:\games\Steam\steamapps\common\Soulash 2',
        "${env:ProgramFiles(x86)}\Steam\steamapps\common\Soulash 2") | Where-Object { $_ }
}
$gameFolder = $candidates | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'SoulashExtender.ini') } | Select-Object -First 1
if (-not $gameFolder) {
    $where = if ($GameDir) { "'$GameDir'" } else { 'this folder, %SOULASH2_GAME_DIR% or the default Steam path' }
    throw "No SoulashExtender.ini found in $where. Copy this tool into the Soulash 2 folder (next to SoulashExtender.ini) or pass -GameDir `"<Soulash 2 folder>`"."
}
$gameFolder = (Resolve-Path -LiteralPath $gameFolder).ProviderPath
$iniPath = Join-Path $gameFolder 'SoulashExtender.ini'

$plugins = Read-IniSection $iniPath 'Plugins'
$pins = Read-IniSection $iniPath 'PluginHashes'
$chain = Read-IniSection $iniPath 'Chain'
$requireOn = $plugins['RequireHash'] -eq '1'
$directoryName = if ($plugins['Directory']) { $plugins['Directory'] } else { 'SoulashExtender\Plugins' }
$pluginFolder = if ([System.IO.Path]::IsPathRooted($directoryName)) { $directoryName } else { Join-Path $gameFolder $directoryName }

Write-Output 'Soulash 2 extender - plugin hash finder'
Write-Output "Game folder   : $gameFolder"
Write-Output "Plugin folder : $pluginFolder"
Write-Output ('RequireHash   : ' + $(if ($requireOn) { 'ON (only plugins listed below with a matching hash load)' } else { 'off (optional; plugins load without a hash check)' }))
Write-Output ''

# --- Hash every plugin -------------------------------------------------------
$found = [ordered]@{}
$skipped = @()
if (Test-Path -LiteralPath $pluginFolder) {
    foreach ($dll in Get-ChildItem -LiteralPath $pluginFolder -Filter *.dll -File | Sort-Object Name) {
        $id = [System.IO.Path]::GetFileNameWithoutExtension($dll.Name)
        if (Test-PluginId $id) { $found[$id] = Get-Sha256 $dll.FullName }
        else { $skipped += $dll.Name }
    }
}
if ($found.Count -eq 0) {
    Write-Output 'No plugin DLLs found; nothing to pin.'
}
else {
    $width = [Math]::Max(6, ($found.Keys | Measure-Object -Property Length -Maximum).Maximum)
    Write-Output (('{0,-' + $width + '}  {1,-8}  {2}') -f 'Plugin', 'Pin', 'SHA-256')
    $pinStates = @{}
    foreach ($id in $found.Keys) {
        $pinnedKey = $pins.Keys | Where-Object { $_ -ieq $id } | Select-Object -First 1
        $state = if (-not $pinnedKey) { 'new' }
            elseif ($pins[$pinnedKey].ToLowerInvariant() -eq $found[$id]) { 'ok' }
            else { 'CHANGED' }
        $pinStates[$id] = $state
        Write-Output (('{0,-' + $width + '}  {1,-8}  {2}') -f $id, $state, $found[$id])
    }
    Write-Output ''
    if (@($pinStates.Values | Where-Object { $_ -eq 'CHANGED' }).Count -gt 0) {
        Write-Output 'CHANGED: the DLL differs from its pin (rebuilt, updated or replaced). Re-pin it only if you trust the new file.'
    }
}
foreach ($name in $skipped) {
    Write-Output "Skipped $name : not a valid plugin id (letters, digits, '.', '_', '-'; must start with a letter or digit). The extender ignores it too."
}
foreach ($key in $pins.Keys) {
    if (-not ($found.Keys | Where-Object { $_ -ieq $key })) {
        Write-Output "Unused pin  : $key (no $key.dll in the plugin folder; harmless, delete the line if the plugin is gone)"
    }
}
if ($chain['Next']) {
    $chainPath = Join-Path $gameFolder $chain['Next']
    if (Test-Path -LiteralPath $chainPath -PathType Leaf) {
        Write-Output "Chain target: $($chain['Next']) -> under [Chain] use NextSha256=$(Get-Sha256 $chainPath) (not written by this tool)"
    }
}

if ($found.Count -gt 0) {
    Write-Output ''
    Write-Output 'Lines for SoulashExtender.ini:'
    Write-Output '[PluginHashes]'
    foreach ($id in $found.Keys) { Write-Output "$id=$($found[$id])" }
    Write-Output ''
}

# --- Optionally write them ---------------------------------------------------
$needsPins = $found.Count -gt 0 -and @($pinStates.Values | Where-Object { $_ -ne 'ok' }).Count -gt 0
$doWrite = $Write -or $RequireHash
$doRequire = [bool]$RequireHash
if ($Prompt -and -not $doWrite) {
    if ($needsPins) { $doWrite = Read-YesNo 'Write these pins into SoulashExtender.ini?' }
    if (-not $requireOn -and $found.Count -gt 0) {
        $doRequire = Read-YesNo 'Also turn hash checking on (RequireHash=1)? Plugins without a matching pin will then be refused'
        $doWrite = $doWrite -or $doRequire
    }
}
if (-not $doWrite) {
    if ($needsPins -and -not $Prompt) { Write-Output 'Report only. Run with -Write to put the pins into SoulashExtender.ini (or -RequireHash to also turn checking on).' }
    exit 0
}

if (Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like 'Soulash*' }) {
    Write-Output 'Note: Soulash 2 is running; the new pins apply from its next start.'
}

# The file as it was before this run becomes SoulashExtender.ini.bak at the end
# (the pin updater keeps its own .bak, which may already include our edits).
$originalBytes = [System.IO.File]::ReadAllBytes($iniPath)

if ($found.Count -gt 0) {
    if ((Find-Section ([System.IO.File]::ReadAllLines($iniPath)) 'PluginHashes') -lt 0) {
        Edit-IniText $iniPath { param($lines) $lines.Add(''); $lines.Add('[PluginHashes]') }
    }
    $pinMap = [ordered]@{}
    foreach ($id in $found.Keys) { $pinMap[$id] = $found[$id] }
    foreach ($change in Update-PluginPins -IniPath $iniPath -Pins $pinMap) {
        if ($change.Action -ne 'unchanged') { Write-Output ('Pin {0,-7} : {1}' -f $change.Action, $change.Name) }
    }
}

if ($doRequire -and -not $requireOn) {
    Edit-IniText $iniPath {
        param($lines)
        $start = Find-Section $lines 'Plugins'
        if ($start -lt 0) {
            $lines.Add(''); $lines.Add('[Plugins]'); $lines.Add('RequireHash=1')
            return
        }
        $end = Find-SectionEnd $lines $start
        for ($i = $start + 1; $i -lt $end; ++$i) {
            if ($lines[$i] -match '^\s*RequireHash\s*=') { $lines[$i] = 'RequireHash=1'; return }
        }
        for ($i = $start + 1; $i -lt $end; ++$i) {
            # The packaged template's commented-out line.
            if ($lines[$i] -match '^\s*;\s*RequireHash\s*=') { $lines[$i] = 'RequireHash=1'; return }
        }
        $insert = $end
        while ($insert - 1 -gt $start -and $lines[$insert - 1].Trim() -eq '') { --$insert }
        $lines.Insert($insert, 'RequireHash=1')
    }
    Write-Output 'RequireHash : turned ON. Re-run this tool after adding or updating any plugin.'
}
$newBytes = [System.IO.File]::ReadAllBytes($iniPath)
if ([System.Linq.Enumerable]::SequenceEqual([byte[]]$originalBytes, [byte[]]$newBytes)) {
    Write-Output 'Nothing to change: SoulashExtender.ini already has these pins.'
}
else {
    [System.IO.File]::WriteAllBytes("$iniPath.bak", $originalBytes)
    Write-Output "Saved       : $iniPath (previous version: SoulashExtender.ini.bak)"
}
