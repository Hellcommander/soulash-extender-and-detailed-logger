<#
.SYNOPSIS
  Re-installs the extender proxy after a Steam update or "verify integrity of
  game files" put the vendor BugSplat64.dll back. Reports only, unless -Apply.

.DESCRIPTION
  Steam replaces BugSplat64.dll with the vendor file on update/verify. The game
  then runs without the extender (nothing breaks, nothing loads). This script:

    * refuses to run while Soulash 2 is open;
    * checks that BugSplat64.dll is the BugSplat-signed vendor file and still
      exports the four MiniDmpSender functions the proxy forwards to (if a new
      BugSplat SDK changed them, the proxy must NOT be installed: it stops);
    * checks that the proxy to install exports the same four functions and is
      not itself a vendor file;
    * with -Apply: backs up an existing BugSplat64_original.dll when the vendor
      file changed (BugSplat64_original.<yyyyMMdd-HHmmss>.bak), moves the vendor
      BugSplat64.dll to BugSplat64_original.dll and copies the proxy in.

  It never touches SoulashExtender.ini, the plugin folder, saves or any other
  game file. The update guard inside the proxy then reports the new game build
  in SoulashExtender\compat-report.txt at the next start.

.EXAMPLE
  .\scripts\repair-install.ps1                         # report what would happen
  .\scripts\repair-install.ps1 -Apply                  # do it
  .\scripts\repair-install.ps1 -ProxyPath .\dist\BugSplat64.dll -Apply
#>
param(
    [string]$GameDir = $(if ($env:SOULASH2_GAME_DIR) { $env:SOULASH2_GAME_DIR } else { 'D:\games\Steam\steamapps\common\Soulash 2' }),
    # The proxy to install. Default: this repository's build output.
    [string]$ProxyPath = (Join-Path (Split-Path -Parent $PSScriptRoot) '.build\BugSplat64.dll'),
    # Also replace an installed proxy that differs from -ProxyPath (an upgrade).
    [switch]$UpdateProxy,
    [switch]$Apply
)

$ErrorActionPreference = 'Stop'

$exports = @(
    '??0MiniDmpSender@@QEAA@PEB_W000K@Z',
    '??1MiniDmpSender@@UEAA@XZ',
    '?sendAdditionalFile@MiniDmpSender@@QEAAXPEB_W@Z',
    '?setGuardByteBufferSize@MiniDmpSender@@QEAAHH@Z'
)

function Get-Sha256([string]$path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
}

function Test-VendorSigned([string]$path) {
    $signature = Get-AuthenticodeSignature -LiteralPath $path
    return $signature.Status -eq 'Valid' -and $signature.SignerCertificate.Subject -match 'BugSplat'
}

# Export names live in the PE export name table as plain ASCII; a byte search is
# enough to notice an SDK that renamed or dropped one of them.
function Get-MissingExports([string]$path) {
    $text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($path))
    return @($exports | Where-Object { -not $text.Contains([string]$_ + [char]0) })
}

if (Get-Process | Where-Object { $_.ProcessName -like 'Soulash*' }) {
    throw 'Soulash 2 is running. Close it first.'
}
$dll = Join-Path $GameDir 'BugSplat64.dll'
$original = Join-Path $GameDir 'BugSplat64_original.dll'
if (-not (Test-Path -LiteralPath (Join-Path $GameDir 'Soulash 2.exe'))) {
    throw "Soulash 2.exe not found in '$GameDir'."
}
if (-not (Test-Path -LiteralPath $dll)) {
    throw 'BugSplat64.dll is missing from the game folder. Verify the game files in Steam first, then run this again.'
}
if (-not (Test-Path -LiteralPath $ProxyPath)) {
    throw "Proxy '$ProxyPath' not found. Build it (scripts\build.ps1) or pass -ProxyPath."
}
if (Test-VendorSigned $ProxyPath) {
    throw "'$ProxyPath' is a signed vendor file, not the extender proxy."
}
$missingInProxy = Get-MissingExports $ProxyPath
if ($missingInProxy.Count -gt 0) {
    throw "'$ProxyPath' does not export $($missingInProxy -join ', '); it is not the extender proxy."
}
$proxyHash = Get-Sha256 $ProxyPath
$dllHash = Get-Sha256 $dll

$actions = New-Object System.Collections.Generic.List[string]
$backup = $null

if (Test-VendorSigned $dll) {
    # The vendor file is back in place: the proxy is not installed.
    $missing = Get-MissingExports $dll
    if ($missing.Count -gt 0) {
        throw ("The vendor BugSplat64.dll no longer exports: $($missing -join ', '). The BugSplat SDK changed; " +
            'the proxy would break the game. Do NOT install it; the proxy must be updated first (docs/update-guard.md).')
    }
    if (Test-Path -LiteralPath $original) {
        $originalHash = Get-Sha256 $original
        if ($originalHash -eq $dllHash) {
            Write-Output 'State        : Steam restored the vendor BugSplat64.dll (same file as BugSplat64_original.dll).'
            $actions.Add('delete BugSplat64.dll (identical to BugSplat64_original.dll)')
        }
        else {
            Write-Output 'State        : Steam installed a NEW vendor BugSplat64.dll (differs from BugSplat64_original.dll).'
            $backup = Join-Path $GameDir ('BugSplat64_original.' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.bak')
            $actions.Add("back up BugSplat64_original.dll -> $(Split-Path -Leaf $backup)")
            $actions.Add('move BugSplat64.dll -> BugSplat64_original.dll (new vendor file)')
        }
    }
    else {
        Write-Output 'State        : vendor BugSplat64.dll in place, no BugSplat64_original.dll (fresh install).'
        $actions.Add('move BugSplat64.dll -> BugSplat64_original.dll')
    }
    $actions.Add("copy proxy $ProxyPath -> BugSplat64.dll")
}
else {
    if (-not (Test-Path -LiteralPath $original)) {
        throw 'BugSplat64.dll is not the signed vendor file and BugSplat64_original.dll is missing. Verify the game files in Steam, then run this again.'
    }
    if (-not (Test-VendorSigned $original)) {
        throw 'BugSplat64_original.dll is not a validly signed BugSplat file. Verify the game files in Steam, then run this again.'
    }
    if ($dllHash -eq $proxyHash) {
        Write-Output 'State        : the extender proxy is installed and identical to the one given. Nothing to do.'
        exit 0
    }
    Write-Output "State        : an extender proxy is installed but differs from '$ProxyPath'."
    if (-not $UpdateProxy) {
        Write-Output 'Nothing to do without -UpdateProxy (that replaces the installed proxy).'
        exit 0
    }
    $actions.Add("replace installed proxy with $ProxyPath")
}

foreach ($action in $actions) { Write-Output "Would        : $action" }
if (-not $Apply) {
    Write-Output 'Report only. Run again with -Apply to make these changes.'
    exit 0
}

if ($backup) {
    Copy-Item -LiteralPath $original -Destination $backup
    Write-Output "Backed up    : $backup"
}
if (Test-VendorSigned $dll) {
    if ((Test-Path -LiteralPath $original) -and -not $backup) {
        [System.IO.File]::Delete($dll)
    }
    else {
        Move-Item -LiteralPath $dll -Destination $original -Force
    }
}
Copy-Item -LiteralPath $ProxyPath -Destination $dll -Force
if ((Get-Sha256 $dll) -ne $proxyHash) {
    throw 'The copied proxy does not match its source hash. Run uninstall.ps1 or verify the game files in Steam.'
}
Write-Output "Installed    : BugSplat64.dll = extender proxy (SHA-256 $proxyHash)"
Write-Output 'Next         : start the game once, then read SoulashExtender\compat-report.txt.'
