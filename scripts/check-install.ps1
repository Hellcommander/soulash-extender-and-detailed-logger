<#
.SYNOPSIS
  Read-only check of a Soulash 2 install: is the extender proxy in place, and is
  it still consistent after a game update?

.DESCRIPTION
  Steam updates replace BugSplat64.dll with the vendor file, silently removing
  the proxy. This script reports that, plus unknown game builds, missing or
  unsigned originals, and plugin / chain files that no longer match the SHA-256
  pins in SoulashExtender.ini. It changes nothing.

.EXAMPLE
  .\scripts\check-install.ps1 -GameDir 'E:\SteamLibrary\steamapps\common\Soulash 2'
#>
param(
    [Parameter(Mandatory = $true)][string]$GameDir
)

$ErrorActionPreference = 'Stop'
$warnings = New-Object System.Collections.Generic.List[string]
$problems = New-Object System.Collections.Generic.List[string]

function Get-Sha256([string]$path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
}

function Read-IniValue([string]$path, [string]$section, [string]$key) {
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    $current = $null
    foreach ($line in Get-Content -LiteralPath $path) {
        $trimmed = $line.Trim()
        if ($trimmed -match '^\[(.+)\]$') { $current = $Matches[1]; continue }
        if ($current -eq $section -and $trimmed -match '^([^=;#]+)=(.*)$' -and $Matches[1].Trim() -eq $key) {
            return $Matches[2].Trim()
        }
    }
    return $null
}

$exePath = Join-Path $GameDir 'Soulash 2.exe'
$proxyPath = Join-Path $GameDir 'BugSplat64.dll'
$originalPath = Join-Path $GameDir 'BugSplat64_original.dll'
$iniPath = Join-Path $GameDir 'SoulashExtender.ini'

if (-not (Test-Path -LiteralPath $exePath)) {
    throw "Soulash 2.exe not found in '$GameDir'."
}

$exeHash = Get-Sha256 $exePath
$versionFile = Join-Path $GameDir 'version.txt'
$version = if (Test-Path -LiteralPath $versionFile) { (Get-Content -LiteralPath $versionFile -TotalCount 1).Trim() } else { 'unknown' }
Write-Output "Game version : $version"
Write-Output "Game exe     : $($exeHash.Substring(0, 16))... (SHA-256 prefix)"

# Known builds recorded in docs/game-notes.md (first 8 hex characters).
$notes = Join-Path (Split-Path -Parent $PSScriptRoot) 'docs\game-notes.md'
if (Test-Path -LiteralPath $notes) {
    $known = [regex]::Matches((Get-Content -LiteralPath $notes -Raw), '`([0-9a-f]{8})`') | ForEach-Object { $_.Groups[1].Value }
    if ($known -notcontains $exeHash.Substring(0, 8)) {
        $warnings.Add("This game build is not in docs/game-notes.md. Anchors, hook addresses and any hash pins made for other builds are unverified here.")
    }
}

# Proxy state.
if (-not (Test-Path -LiteralPath $originalPath)) {
    $problems.Add('BugSplat64_original.dll is missing. The proxy cannot work without the renamed vendor DLL.')
}
elseif (-not (Test-Path -LiteralPath $proxyPath)) {
    $problems.Add('BugSplat64.dll is missing; the game will not start.')
}
else {
    $originalSignature = Get-AuthenticodeSignature -LiteralPath $originalPath
    if ($originalSignature.Status -ne 'Valid' -or $originalSignature.SignerCertificate.Subject -notmatch 'BugSplat') {
        $problems.Add("BugSplat64_original.dll does not carry a valid BugSplat signature ($($originalSignature.Status)). Do not use a DLL from another source.")
    }
    if ((Get-Sha256 $proxyPath) -eq (Get-Sha256 $originalPath)) {
        $warnings.Add("EXTENDER NOT INSTALLED: BugSplat64.dll is the vendor file. A game update or 'verify integrity' restores it. Re-install the proxy (see README, 'After a game update').")
    }
    else {
        $proxySignature = Get-AuthenticodeSignature -LiteralPath $proxyPath
        if ($proxySignature.Status -eq 'Valid' -and $proxySignature.SignerCertificate.Subject -match 'BugSplat') {
            $warnings.Add('BugSplat64.dll carries a BugSplat signature but differs from BugSplat64_original.dll: the vendor DLL was updated. Re-install the proxy over the new vendor DLL and refresh BugSplat64_original.dll.')
        }
        else {
            Write-Output 'Proxy        : BugSplat64.dll is not the vendor file (extender proxy installed)'
        }
    }
}

# Pins in the INI.
if (Test-Path -LiteralPath $iniPath) {
    $requireHash = (Read-IniValue $iniPath 'Plugins' 'RequireHash') -eq '1'
    $pluginDirectoryName = Read-IniValue $iniPath 'Plugins' 'Directory'
    if (-not $pluginDirectoryName) { $pluginDirectoryName = 'SoulashExtender\Plugins' }
    $pluginDirectory = if ([System.IO.Path]::IsPathRooted($pluginDirectoryName)) { $pluginDirectoryName } else { Join-Path $GameDir $pluginDirectoryName }
    if ((Read-IniValue $iniPath 'Plugins' 'Enabled') -eq '1' -and (Test-Path -LiteralPath $pluginDirectory)) {
        foreach ($dll in Get-ChildItem -LiteralPath $pluginDirectory -Filter *.dll -File) {
            $id = [System.IO.Path]::GetFileNameWithoutExtension($dll.Name)
            $pinned = Read-IniValue $iniPath 'PluginHashes' $id
            if ($requireHash -and -not $pinned) {
                $warnings.Add("Plugin '$id' has no [PluginHashes] entry and will be refused (RequireHash=1).")
            }
            elseif ($pinned -and $pinned.ToLowerInvariant() -ne (Get-Sha256 $dll.FullName)) {
                $warnings.Add("Plugin '$id' does not match its pinned SHA-256 and will be refused.")
            }
        }
    }
    $chainNext = Read-IniValue $iniPath 'Chain' 'Next'
    if ($chainNext) {
        $chainPath = Join-Path $GameDir $chainNext
        $chainPin = Read-IniValue $iniPath 'Chain' 'NextSha256'
        if (-not (Test-Path -LiteralPath $chainPath)) {
            $problems.Add("Chain target '$chainNext' is missing; the game will fail fast at start-up.")
        }
        elseif ($chainPin -and $chainPin.ToLowerInvariant() -ne (Get-Sha256 $chainPath)) {
            $problems.Add("Chain target '$chainNext' does not match [Chain] NextSha256; the game will fail fast at start-up.")
        }
        elseif ($requireHash -and -not $chainPin) {
            $problems.Add("Chain target '$chainNext' has no NextSha256 but RequireHash=1; the game will fail fast at start-up.")
        }
    }
}
else {
    $warnings.Add('SoulashExtender.ini is missing: diagnostics and plugins are disabled.')
}

foreach ($message in $problems) { Write-Output "PROBLEM      : $message" }
foreach ($message in $warnings) { Write-Output "WARNING      : $message" }
if ($problems.Count -eq 0 -and $warnings.Count -eq 0) { Write-Output 'OK           : no issues found.' }
exit ([int]($problems.Count -gt 0))
