<#
.SYNOPSIS
  Downloads the official Lua 5.4.9 source from lua.org, verifies its SHA-256,
  and unpacks only the src folder into third_party\lua-5.4.9\.

.DESCRIPTION
  The Lua sources are NOT part of this repository. Building the extender
  requires them. Official sources:
    Download page : https://www.lua.org/ftp/
    Tarball       : https://www.lua.org/ftp/lua-5.4.9.tar.gz
    Expected hash : 2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6
  You can also download and unpack it yourself so that
  third_party\lua-5.4.9\src\lua.h exists.

  Nothing outside the repository's third_party folder is changed. The download
  is unpacked in a fresh temporary folder; only src is copied.
#>
$ErrorActionPreference = 'Stop'

$version = '5.4.9'
$url = "https://www.lua.org/ftp/lua-$version.tar.gz"
$expectedHash = '2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6'

$repoRoot = Split-Path -Parent $PSScriptRoot
$destination = Join-Path $repoRoot "third_party\lua-$version"
if (Test-Path -LiteralPath (Join-Path $destination 'src\lua.h')) {
    Write-Output "Lua $version is already present in $destination"
    return
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("soulash-lua-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
$archive = Join-Path $work "lua-$version.tar.gz"
Write-Output "Downloading $url"
Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing

$actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
if ($actual -ne $expectedHash) {
    throw "SHA-256 mismatch for $url`n  expected $expectedHash`n  actual   $actual`nNothing was unpacked. Compare with https://www.lua.org/ftp/ before changing this script."
}

$unpacked = Join-Path $work 'unpacked'
New-Item -ItemType Directory -Path $unpacked | Out-Null
tar -xzf $archive -C $unpacked
if ($LASTEXITCODE -ne 0) { throw 'tar failed to unpack the Lua archive.' }

$source = Join-Path $unpacked "lua-$version\src"
if (-not (Test-Path -LiteralPath (Join-Path $source 'lua.h'))) { throw 'Unexpected archive layout.' }

New-Item -ItemType Directory -Path $destination -Force | Out-Null
Copy-Item -LiteralPath $source -Destination $destination -Recurse -Force
Write-Output "Lua $version unpacked to $destination (SHA-256 verified)."
