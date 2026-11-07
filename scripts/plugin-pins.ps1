# Dot-source this file: defines Update-PluginPins.
#
# Updates ONLY the given keys in the [PluginHashes] section of a
# SoulashExtender.ini. Every other section, key, comment and the file's line
# ending style stay as they are; pins for plugins not given are left alone.
# The file is replaced atomically (File.Replace) with the previous version kept
# as <ini>.bak. A missing file or a missing [PluginHashes] section is an error:
# nothing is created. Owner-authorised (2026-11-05) for the extender's own
# SoulashExtender.ini in the game folder: the build calls this right after it
# copied the plugins into <game>\SoulashExtender\Plugins.

function Update-PluginPins {
    param(
        [Parameter(Mandatory = $true)][string]$IniPath,
        # Ordered name -> lower-case SHA-256 of the DLL as copied.
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$Pins
    )

    foreach ($name in $Pins.Keys) {
        if ($name -notmatch '^[A-Za-z0-9._-]{1,128}$') { throw "Plugin id '$name' is not a plain file stem." }
        if ($Pins[$name] -notmatch '^[0-9a-f]{64}$') { throw "Pin for '$name' is not 64 lower-case hex characters." }
    }
    if (-not (Test-Path -LiteralPath $IniPath -PathType Leaf)) {
        throw "$IniPath does not exist; not creating it. Paste the pins by hand."
    }

    $bytes = [System.IO.File]::ReadAllBytes($IniPath)
    $hasBom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    $text = [System.Text.Encoding]::UTF8.GetString($bytes, $(if ($hasBom) { 3 } else { 0 }), $bytes.Length - $(if ($hasBom) { 3 } else { 0 }))
    $newline = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
    $endsWithNewline = $text.EndsWith("`n")
    $lines = [System.Collections.Generic.List[string]]::new()
    foreach ($line in ($text -split "`r?`n")) { $lines.Add($line) }
    if ($endsWithNewline) { $lines.RemoveAt($lines.Count - 1) }

    $start = -1
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -match '^\s*\[\s*PluginHashes\s*\]\s*$') { $start = $i; break }
    }
    if ($start -lt 0) {
        throw "$IniPath has no [PluginHashes] section; not creating one. Paste the pins by hand."
    }
    $end = $lines.Count   # exclusive
    for ($i = $start + 1; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -match '^\s*\[') { $end = $i; break }
    }

    $changes = @()
    foreach ($name in $Pins.Keys) {
        $new = $Pins[$name]
        $found = -1
        for ($i = $start + 1; $i -lt $end; ++$i) {
            if ($lines[$i] -match '^\s*([^=;#\s][^=]*?)\s*=\s*(.*?)\s*$' -and $Matches[1] -ieq $name) { $found = $i; $old = $Matches[2]; break }
        }
        if ($found -ge 0) {
            if ($old.ToLowerInvariant() -eq $new) {
                $changes += [pscustomobject]@{ Name = $name; Old = $old; New = $new; Action = 'unchanged' }
                continue
            }
            $key = ($lines[$found] -split '=', 2)[0].TrimEnd()
            $lines[$found] = "$key=$new"
            $changes += [pscustomobject]@{ Name = $name; Old = $old; New = $new; Action = 'updated' }
        }
        else {
            # Append after the section's last non-blank line.
            $insert = $end
            while ($insert - 1 -gt $start -and $lines[$insert - 1].Trim() -eq '') { --$insert }
            $lines.Insert($insert, "$name=$new")
            ++$end
            $changes += [pscustomobject]@{ Name = $name; Old = ''; New = $new; Action = 'added' }
        }
    }

    if (@($changes | Where-Object { $_.Action -ne 'unchanged' }).Count -gt 0) {
        $output = [string]::Join($newline, $lines) + $(if ($endsWithNewline) { $newline } else { '' })
        $encoded = [System.Text.Encoding]::UTF8.GetBytes($output)
        if ($hasBom) { $encoded = [byte[]](0xEF, 0xBB, 0xBF) + $encoded }
        $full = (Resolve-Path -LiteralPath $IniPath).ProviderPath
        $temporary = "$full.tmp"
        [System.IO.File]::WriteAllBytes($temporary, $encoded)
        [System.IO.File]::Replace($temporary, $full, "$full.bak")
    }
    return $changes
}
