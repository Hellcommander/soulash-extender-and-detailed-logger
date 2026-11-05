# Update guard: surviving game updates

A Soulash 2 update can hurt the extender in three ways:

| What the update does | Effect | What handles it |
|---|---|---|
| Steam restores the vendor `BugSplat64.dll` (every update and every "verify integrity") | The extender is silently gone; the game runs unmodded | `scripts\check-install.ps1` notices, `scripts\repair-install.ps1` puts the proxy back |
| The game's code changes | Plugins that rely on code layouts may not find what they need, or, if they patch blindly, crash the game | Plugins verify layouts and report their status; the **compatibility report** lists them; **crash-loop safe mode** stops a broken plugin from crashing every start |
| You rebuild or replace a plugin | Its SHA-256 pin no longer matches, so it is refused | The compatibility report says which plugin was refused and why |

The guard is part of the proxy (`src/update_guard.cpp`, logic in
`src/update_guard_logic.hpp`). It does **not** need the game to be any particular
build, and it never touches game files or saves.

## What happens at every start

On the plugin loader thread, before any native plugin loads:

1. **Build detection.** The game exe's SHA-256 (already computed for plugins),
   `version.txt` and the SHA-256 of `BugSplat64_original.dll` are compared with
   the previous session's record. A different exe is logged as
   `update guard: GAME UPDATED: 0.10.5.1 60b72c10 -> ...`, and a replaced
   vendor DLL is logged too. The guard remembers the last 16 builds it has seen,
   so it can tell a first run on a new build from a rollback.
2. **Safe-mode decision** (see below).
3. Plugins load as usual. Each outcome is recorded: `loaded`, `REFUSED: its
   SHA-256 is not listed in [PluginHashes]`, `LOAD FAILED: Windows error N`,
   `FAILED: its load function returned failure`, or `SKIPPED: safe mode`.
4. After POSTPOSTLOAD the guard writes
   `<game>\SoulashExtender\compat-report.txt` and logs a one-line summary
   (`N plugin(s) inactive or degraded on this build: ...`).

Example report:

```
Soulash Extender compatibility report (rewritten at every start; docs/update-guard.md)
written_utc=2026-11-05T11:23:59Z
game_version=0.10.6.0
game_sha256=...
game_build=UPDATED since the last start (was 0.10.5.1 60b72c10); check every plugin below
first_run_on_this_build=yes
vendor_bugsplat_sha256=4016763c...
previous_session=clean exit, native plugins were loaded
unclean_streak=0
safe_mode=no
known_builds=924e1ef3 0.10.4.x (first seen ...), 60b72c10 0.10.5.1 (first seen ...), ...
plugins=3 found, 3 loaded
  AbilityBarProbe: loaded; INACTIVE on this build: inactive: game build 1a2b3c4d does not match the verified ability-bar layout (...)
  AbilityBarWheel: loaded; disabled by its config: Enabled=0 in AbilityBarWheel.ini
  TestBot: loaded; DEGRADED on this build: achievements blocked, bot NOT running: ...
summary=0 active, 2 need attention
end of report
```

If a plugin reports a new status later in the session (for example it stops
itself after a memory fault), the report is rewritten.

## Crash-loop safe mode

The guard records whether each session ended cleanly. When **2** sessions in a
row (by default) ended without a clean exit **while native plugins were loaded**,
the next start loads **no native plugins**. Lua scripts, conditions and effects
files still load, because they are sandboxed or plain data and cannot crash the
game the same way. The report then says `safe_mode=YES: ...`.

* Leave safe mode by exiting the game normally once. The streak then resets and
  plugins load again at the next start.
* A new game build always gets a fresh streak.
* An unclean session without native plugins neither counts nor clears the streak.

A session counts as **clean** when either:

* the game destroys its BugSplat crash reporter (the proxy's `MiniDmpSender`
  destructor), which is part of its orderly shutdown; or
* the process ends through `ExitProcess`. The proxy's `DllMain` sees
  `DLL_PROCESS_DETACH` with a non-NULL `lpReserved`.

A crash ends the process through the crash reporter or `TerminateProcess`, and
neither of those reaches the proxy, so the session stays unclean. Killing the
game from Task Manager also counts as unclean.

> **Not verified in the real game yet.** The tests cover both clean-exit paths
> in a test process: the destructor path in the plugin-directory test, and the
> process-exit path in the allowlist test. Whether Soulash 2's own shutdown
> reaches either path has not been observed. After your first normal quit,
> `SoulashExtender\UpdateGuard.state` should say `clean_exit=1`. If it says `0`
> after normal quits, set `SafeModeAfterUncleanExits=0` until this is fixed.
> Otherwise every second start would be in safe mode.

## Configuration (`SoulashExtender.ini`)

```ini
[UpdateGuard]
Enabled=1                    ; 0 = no detection, no safe mode, no report
SafeModeAfterUncleanExits=2  ; 0 = never enter safe mode automatically
ForceSafeMode=0              ; 1 = start without native plugins (bisecting a crash)
```

All keys are optional, and the defaults are the values shown.

## Files

| File | Purpose |
|---|---|
| `SoulashExtender\UpdateGuard.state` | The guard's record (format=1 key=value; rewritten atomically). Deleting it is safe: the next start is treated as a first start. |
| `SoulashExtender\compat-report.txt` | Human-readable report, rewritten at every start. |

## After a game update: checklist

1. Close the game. Run `scripts\check-install.ps1 -GameDir "<Soulash 2 folder>"`.
2. If it reports `EXTENDER NOT INSTALLED`, run `scripts\repair-install.ps1`.
   It prints what it would do. Run it again with `-Apply` to do it. It
   refuses to install if the new vendor DLL no longer exports the four
   `MiniDmpSender` functions the proxy forwards to. It backs up an
   older `BugSplat64_original.dll` when the vendor file itself changed.
3. Start the game once and read `SoulashExtender\compat-report.txt`.
   `game_build=UPDATED` plus each plugin's status tells you what still works.
4. Plugins that report `INACTIVE` need re-verifying for the new exe
   (`docs/game-notes.md`). They have installed nothing, so they are harmless
   until then.

To upgrade an installed proxy to a newer build, run
`scripts\repair-install.ps1 -UpdateProxy` (then `-UpdateProxy -Apply`).

## For plugin authors: `reportStatus`

Interface v3 gained `reportStatus` at its end. Older proxies do not have it, so
check with `SOULASH_EXTENDER_HAS`, and accept the shorter interface by
comparing `structureSize` with `SOULASH_EXTENDER_INTERFACE_V3_BASE_SIZE`
instead of `sizeof`:

```c
if (extender->structureSize < SOULASH_EXTENDER_INTERFACE_V3_BASE_SIZE) return FALSE;
...
if (SOULASH_EXTENDER_HAS(extender, reportStatus))
    extender->reportStatus(extender->pluginHandle, SOULASH_PLUGIN_STATUS_INACTIVE,
                           "DrawActionBar layout not recognised on this build");
```

| Status | Meaning |
|---|---|
| `SOULASH_PLUGIN_STATUS_ACTIVE` | Working on this build |
| `SOULASH_PLUGIN_STATUS_INACTIVE` | Did not recognise this build; installed nothing |
| `SOULASH_PLUGIN_STATUS_DISABLED` | Turned off by its own config |
| `SOULASH_PLUGIN_STATUS_DEGRADED` | Partly working (say what is missing) |

The detail text is truncated to 200 printable ASCII characters. Status may be
reported again at any time, and the latest one wins. The repository's plugins use
`plugins/common/plugin_status.hpp`. It wraps the availability check and turns the
plugin's last log line during load into the reason for a failed install.

A plugin that cannot verify the code it patches should report `INACTIVE` and do
nothing. That is what keeps a game update from turning into a crash. Safe mode
is only the backstop for plugins that get this wrong.
