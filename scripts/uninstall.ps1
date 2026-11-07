param(
    [string]$GameDir = $(if ($env:SOULASH2_GAME_DIR) { $env:SOULASH2_GAME_DIR } else { 'D:\games\Steam\steamapps\common\Soulash 2' }),
    # SHA-256 of the vendor BugSplat64.dll recorded before installing; verified after the rename.
    [string]$ExpectedVendorSha256 = ''
)
# Rolls back the proxy install: removes the proxy BugSplat64.dll and renames
# BugSplat64_original.dll back. Leaves SoulashExtender.ini, SoulashExtender\
# and logs\ alone (they are inert without the proxy). Refuses to run while the
# game is open. Nothing else in the game folder is touched.
$ErrorActionPreference = 'Stop'

if (Get-Process | Where-Object { $_.ProcessName -like 'Soulash*' }) {
    throw 'Soulash 2 is running. Close it first.'
}
$proxy = Join-Path $GameDir 'BugSplat64.dll'
$original = Join-Path $GameDir 'BugSplat64_original.dll'
if (-not (Test-Path -LiteralPath $original)) {
    throw "No BugSplat64_original.dll in $GameDir: nothing to roll back (the vendor file is in place if check-install.ps1 says EXTENDER NOT INSTALLED)."
}
$signature = Get-AuthenticodeSignature -LiteralPath $original
if ($signature.Status -ne 'Valid') {
    throw "BugSplat64_original.dll is not a validly signed vendor file (status $($signature.Status)); stopping. Resolve by hand."
}
if (Test-Path -LiteralPath $proxy) {
    $proxySignature = Get-AuthenticodeSignature -LiteralPath $proxy
    if ($proxySignature.Status -eq 'Valid') {
        throw 'BugSplat64.dll is itself a signed vendor file; refusing to delete it. Resolve by hand.'
    }
    [System.IO.File]::Delete($proxy)
    Write-Output "Removed proxy $proxy"
}
[System.IO.File]::Move($original, $proxy)
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $proxy).Hash
Write-Output "Restored vendor BugSplat64.dll (SHA-256 $hash)"
if ($ExpectedVendorSha256 -and $hash -ne $ExpectedVendorSha256.ToUpperInvariant()) {
    throw "Restored file hash does not match the recorded vendor hash $ExpectedVendorSha256."
}
Write-Output 'Rollback complete. Run check-install.ps1 to confirm: it should report EXTENDER NOT INSTALLED.'
